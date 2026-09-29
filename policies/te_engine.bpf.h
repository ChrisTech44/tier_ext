/* SPDX-License-Identifier: GPL-2.0 */
/*
 * te_engine.bpf.h: building blocks for "engine" policies, i.e. tier_ext
 * policies that keep their own ordering of folios and choose victims with
 * pick_victims (the cache_ext model, for anon and file, per lruvec).
 *
 * State (see design/engines.md):
 *
 *   te_nodes[pfn]	one 16-byte node per page frame, in a BPF arena. A
 *			node is on at most one list. Links hold "pfn + 1", so
 *			zeroed memory means "not linked" / "empty list".
 *   te_lruvecs[nid]	per NUMA node (= per lruvec, one policy covers one
 *			memcg): a spin lock, TE_NR_LISTS list heads and the
 *			policy's own struct te_extra, all protected by the lock.
 *
 * Every list operation must hold the node's lock (te_lock()/te_unlock()).
 * Under a bpf_spin_lock a program may not call helpers, only touch memory
 * and call spin-lock-safe kfuncs such as bpf_tier_ext_add_victim(), so the
 * whole library is plain loads and stores.
 *
 * A policy includes tier_ext.bpf.h, then defines
 *
 *   #define TE_NR_LISTS n		lists per lruvec
 *   struct te_extra { ... };		per-lruvec policy state (may be empty)
 *
 * and then includes this file.
 *
 * The node array is allocated by the te_init syscall program, which the
 * loader runs with the highest pfn of the machine before attaching.
 */
#ifndef __TE_ENGINE_BPF_H
#define __TE_ENGINE_BPF_H

#include "tier_ext.bpf.h"

#ifndef TE_NR_LISTS
#error "define TE_NR_LISTS before including te_engine.bpf.h"
#endif

#define __arena __attribute__((address_space(1)))
#define __arena_global __attribute__((address_space(1)))

#ifndef NUMA_NO_NODE
#define NUMA_NO_NODE (-1)
#endif

void __arena *bpf_arena_alloc_pages(void *map, void __arena *addr,
				    __u32 page_cnt, int node_id,
				    __u64 flags) __ksym __weak;

/* Arena: up to 4 GiB of virtual space, i.e. nodes for 2^28 pfns (1 TiB). */
#define TE_ARENA_PAGES	(1U << 20)
#define TE_MAX_PFN	((u64)TE_ARENA_PAGES * 4096 / 16)

struct {
	__uint(type, BPF_MAP_TYPE_ARENA);
	__uint(map_flags, BPF_F_MMAPABLE);
	__uint(max_entries, TE_ARENA_PAGES);
} te_arena SEC(".maps");

struct te_node {
	u32 prev;	/* pfn + 1 of the neighbour towards the head, 0: none */
	u32 next;	/* pfn + 1 of the neighbour towards the tail, 0: none */
	u16 list;	/* list index + 1, 0: not on a list */
	u8 order;	/* folio order when it was linked */
	u8 flags;	/* policy-defined */
	u32 meta;	/* policy-defined */
};

/* Head = most recently inserted end, tail = the end victims come from. */
struct te_list {
	u32 head;
	u32 tail;
	u64 nr;		/* folios */
	u64 nr_pages;	/* sum of 2^order of the linked nodes */
};

struct te_lruvec {
	struct bpf_spin_lock lock;
	struct te_list lists[TE_NR_LISTS];
	struct te_extra x;
};

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, TE_MAX_NODES);
	__type(key, u32);
	__type(value, struct te_lruvec);
} te_lruvecs SEC(".maps");

/* Arena globals, so every program that uses them is tied to the arena. */
struct te_node __arena *__arena_global te_nodes;
u64 __arena_global te_max_pfn;

/* ------------------------------------------------------------------ init */

struct te_init_args {
	u64 max_pfn;	/* one past the highest pfn of the machine */
};

SEC("syscall")
int te_init(struct te_init_args *args)
{
	struct te_node __arena *nodes;
	u64 max_pfn = args->max_pfn, pages;

	if (te_nodes)
		return 0;
	if (!max_pfn || max_pfn > TE_MAX_PFN)
		return -22;	/* EINVAL */
	pages = (max_pfn * sizeof(struct te_node) + 4095) / 4096;
	nodes = bpf_arena_alloc_pages(&te_arena, NULL, pages, NUMA_NO_NODE, 0);
	if (!nodes)
		return -12;	/* ENOMEM */
	te_nodes = nodes;
	te_max_pfn = max_pfn;
	return 0;
}

/* ------------------------------------------------------------- accessors */

static __always_inline struct te_node __arena *te_node(u64 pfn)
{
	struct te_node __arena *nodes = te_nodes;

	if (!nodes || pfn >= te_max_pfn)
		return NULL;
	return &nodes[pfn];
}

/* Node from a link value (pfn + 1). */
static __always_inline struct te_node __arena *te_link(u32 link)
{
	return link ? te_node((u64)link - 1) : NULL;
}

static __always_inline struct te_lruvec *te_lv(s32 nid)
{
	u32 key = nid;

	return bpf_map_lookup_elem(&te_lruvecs, &key);
}

static __always_inline struct te_list *te_list(struct te_lruvec *lv, u32 id)
{
	if (id >= TE_NR_LISTS)
		return NULL;
	return &lv->lists[id];
}

#define te_lock(lv)	bpf_spin_lock(&(lv)->lock)
#define te_unlock(lv)	bpf_spin_unlock(&(lv)->lock)

/* Order of a folio of @nr_pages pages (a power of two). */
static __always_inline u8 te_order(u32 nr_pages)
{
	u8 order = 0;

	if (nr_pages >= 65536) { nr_pages >>= 16; order += 16; }
	if (nr_pages >= 256) { nr_pages >>= 8; order += 8; }
	if (nr_pages >= 16) { nr_pages >>= 4; order += 4; }
	if (nr_pages >= 4) { nr_pages >>= 2; order += 2; }
	if (nr_pages >= 2) order += 1;
	return order;
}

static __always_inline u64 te_node_pages(struct te_node __arena *n)
{
	return 1ULL << (n->order & 31);
}

/* --------------------------------------------- list operations, locked */

/* Index of the list @n is on, or -1. */
static __always_inline int te_list_id(struct te_node __arena *n)
{
	return (int)n->list - 1;
}

static __always_inline void te_unlink(struct te_lruvec *lv,
				      struct te_node __arena *n)
{
	struct te_node __arena *pn, *nn;
	struct te_list *l;
	u32 id = n->list;

	if (!id)
		return;
	n->list = 0;
	l = te_list(lv, id - 1);
	if (!l)
		return;
	pn = te_link(n->prev);
	nn = te_link(n->next);
	if (pn)
		pn->next = n->next;
	else
		l->head = n->next;
	if (nn)
		nn->prev = n->prev;
	else
		l->tail = n->prev;
	n->prev = n->next = 0;
	l->nr--;
	l->nr_pages -= te_node_pages(n);
}

static __always_inline void te_push_head(struct te_lruvec *lv, u32 id,
					 u64 pfn, struct te_node __arena *n)
{
	struct te_node __arena *h;
	struct te_list *l;

	te_unlink(lv, n);
	l = te_list(lv, id);
	if (!l)
		return;
	n->prev = 0;
	n->next = l->head;
	h = te_link(l->head);
	if (h)
		h->prev = pfn + 1;
	else
		l->tail = pfn + 1;
	l->head = pfn + 1;
	n->list = id + 1;
	l->nr++;
	l->nr_pages += te_node_pages(n);
}

static __always_inline void te_push_tail(struct te_lruvec *lv, u32 id,
					 u64 pfn, struct te_node __arena *n)
{
	struct te_node __arena *t;
	struct te_list *l;

	te_unlink(lv, n);
	l = te_list(lv, id);
	if (!l)
		return;
	n->next = 0;
	n->prev = l->tail;
	t = te_link(l->tail);
	if (t)
		t->next = pfn + 1;
	else
		l->head = pfn + 1;
	l->tail = pfn + 1;
	n->list = id + 1;
	l->nr++;
	l->nr_pages += te_node_pages(n);
}

/*
 * Let @n (at @new_pfn) take the place of @o (at @old_pfn) in its list, for
 * migration: the data keeps its position.
 */
static __always_inline void te_replace(struct te_lruvec *lv,
				       struct te_node __arena *o,
				       u64 new_pfn, struct te_node __arena *n)
{
	struct te_node __arena *pn, *nn;
	struct te_list *l;
	u32 id = o->list;

	te_unlink(lv, n);
	if (!id)
		return;
	l = te_list(lv, id - 1);
	if (!l)
		return;
	n->prev = o->prev;
	n->next = o->next;
	n->list = id;
	n->order = o->order;
	n->flags = o->flags;
	n->meta = o->meta;
	pn = te_link(o->prev);
	nn = te_link(o->next);
	if (pn)
		pn->next = new_pfn + 1;
	else
		l->head = new_pfn + 1;
	if (nn)
		nn->prev = new_pfn + 1;
	else
		l->tail = new_pfn + 1;
	o->list = 0;
	o->prev = o->next = 0;
}

/* ----------------------------------------------------- common handlers */

/* Forget a folio (evicted, removed): unlink its node. */
static __always_inline void te_forget(s32 nid, u64 pfn)
{
	struct te_node __arena *n = te_node(pfn);
	struct te_lruvec *lv = te_lv(nid);

	if (!n || !lv)
		return;
	te_lock(lv);
	te_unlink(lv, n);
	te_unlock(lv);
}

/*
 * Migration: the old pfn's node hands its position to the new pfn when both
 * are on the same node (compaction); otherwise it is forgotten and the new
 * folio arrives through folio_added on its own lruvec.
 */
static __always_inline void te_migrate(struct tier_ext_folio_ctx *ctx)
{
	struct te_node __arena *o = te_node(ctx->old_pfn);
	struct te_node __arena *n = te_node(ctx->pfn);
	struct te_lruvec *lv = te_lv(ctx->old_nid);

	if (!o || !lv)
		return;
	te_lock(lv);
	if (ctx->old_nid == ctx->nid && n)
		te_replace(lv, o, ctx->pfn, n);
	else
		te_unlink(lv, o);
	te_unlock(lv);
}

/*
 * Nominate @n (at @pfn) as a victim: unlink it and hand it to the kernel.
 * A nominated folio belongs to the kernel until it is either evicted
 * (folio_evicted) or handed back (folio_added with TIER_EXT_F_RETURNED).
 * Returns the folio's pages, or 0 if the batch is full.
 */
static __always_inline u64 te_nominate(struct tier_ext_victims_ctx *vctx,
				       struct te_lruvec *lv, u64 pfn,
				       struct te_node __arena *n)
{
	if (bpf_tier_ext_add_victim(vctx, pfn))
		return 0;
	te_unlink(lv, n);
	return te_node_pages(n);
}

static __always_inline bool te_returned(const struct tier_ext_folio_ctx *ctx)
{
	return ctx->folio_flags & TIER_EXT_F_RETURNED;
}

/* Type index: 0 anon, 1 file (the kernel's LRU_GEN_ANON/LRU_GEN_FILE). */
static __always_inline int te_type(const struct tier_ext_folio_ctx *ctx)
{
	return !(ctx->folio_flags & TIER_EXT_F_ANON);
}

/* Can pick_victims take folios of @type in this batch? */
static __always_inline bool te_may(const struct tier_ext_victims_ctx *v,
				   int type)
{
	return v->flags & (type ? TIER_EXT_VF_MAY_FILE : TIER_EXT_VF_MAY_ANON);
}

#endif /* __TE_ENGINE_BPF_H */
