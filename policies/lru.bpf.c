// SPDX-License-Identifier: GPL-2.0
/*
 * lru: the kernel's classic two-list LRU, re-implemented as a tier_ext engine
 * for page cache *and* anon memory.
 *
 * Per lruvec (node) there are four lists, (anon, file) x (inactive, active),
 * kept in BPF (te_engine.bpf.h), and the policy chooses every victim itself:
 *
 *   folio_added	new folios go to the head of the inactive list (active
 *			if the kernel marked them active, e.g. workingset
 *			refaults). Folios handed back after a failed reclaim
 *			go where the kernel's verdict put them: active if it
 *			activated them (young PTEs), inactive otherwise.
 *   folio_accessed	folio_mark_accessed(): the first access sets a
 *			referenced bit, the second one activates an inactive
 *			folio (exactly mm/folio.c's rule).
 *   folio_referenced	(MGLRU look-around) same as an access.
 *   pick_victims	splits the batch between anon and file like
 *			get_scan_count() (the kernel's own split when the
 *			classic engine runs, swappiness x size otherwise),
 *			refills an inactive list from its active list when
 *			inactive_is_low() (same sqrt(10 x GB) ratio), and
 *			nominates from the inactive tail.
 *
 * Page table accesses are found by the kernel when it reclaims a nominated
 * folio (folio_referenced() in shrink_folio_list()): referenced anon and
 * shared file folios are activated and come back with TIER_EXT_F_ACTIVE,
 * which is the classic LRU's second chance.
 */
#include "tier_ext.bpf.h"

#define TE_NR_LISTS 4

struct te_extra {
	u64 picked[2];		/* pages nominated this pass, per type */
};

#include "te_engine.bpf.h"

char _license[] SEC("license") = "GPL";

#define LIST(type, active)	((type) * 2 + (active))
#define F_REF			0x1	/* PG_referenced, the policy's copy */

u64 nr_added, nr_returned, nr_returned_active, nr_activated, nr_deactivated;
u64 nr_pick[2];

static __always_inline bool is_active_list(int id)
{
	return id & 1;
}

TIER_EXT_EVENT(folio_added)
{
	struct te_node __arena *n = te_node(ctx->pfn);
	struct te_lruvec *lv = te_lv(ctx->nid);
	int type = te_type(ctx);
	bool active = ctx->folio_flags & TIER_EXT_F_ACTIVE;
	bool returned = te_returned(ctx);

	if (!n || !lv)
		return;
	te_lock(lv);
	if (ctx->folio_flags & TIER_EXT_F_UNEVICTABLE) {
		te_unlink(lv, n);
	} else if (returned || !n->list) {
		n->order = te_order(ctx->nr_pages);
		n->flags = 0;
		te_push_head(lv, LIST(type, active), ctx->pfn, n);
	}
	te_unlock(lv);

	if (!returned)
		__sync_fetch_and_add(&nr_added, 1);
	else if (active)
		__sync_fetch_and_add(&nr_returned_active, 1);
	else
		__sync_fetch_and_add(&nr_returned, 1);
}

/* folio_mark_accessed() for the classic LRU */
static __always_inline void mark_accessed(struct tier_ext_folio_ctx *ctx)
{
	struct te_node __arena *n = te_node(ctx->pfn);
	struct te_lruvec *lv = te_lv(ctx->nid);
	bool activated = false;
	int id;

	if (!n || !lv)
		return;
	te_lock(lv);
	id = te_list_id(n);
	if (id >= 0) {
		if (!(n->flags & F_REF)) {
			n->flags |= F_REF;
		} else if (!is_active_list(id)) {
			n->flags &= ~F_REF;
			te_push_head(lv, id | 1, ctx->pfn, n);
			activated = true;
		}
	}
	te_unlock(lv);
	if (activated)
		__sync_fetch_and_add(&nr_activated, 1);
}

TIER_EXT_EVENT(folio_accessed)
{
	mark_accessed(ctx);
}

TIER_EXT_EVENT(folio_referenced)
{
	mark_accessed(ctx);
}

TIER_EXT_EVENT(folio_evicted)
{
	te_forget(ctx->nid, ctx->pfn);
}

TIER_EXT_EVENT(folio_removed)
{
	te_forget(ctx->nid, ctx->pfn);
}

TIER_EXT_EVENT(folio_migrated)
{
	te_migrate(ctx);
}

/*
 * int_sqrt() for the inactive ratio. An unrolled bit-by-bit square root
 * branches 2^16 ways in the verifier; a bounded search converges, and the
 * answer is small (31 for 100 GiB).
 */
static __always_inline u64 isqrt(u64 x)
{
	u64 r = 1;
	int i;

	bpf_for(i, 2, 4096) {
		if ((u64)i * i > x)
			break;
		r = i;
	}
	return r;
}

/* mm/vmscan.c: inactive_is_low() */
static __always_inline bool inactive_is_low(struct te_lruvec *lv, int type)
{
	u64 inactive = lv->lists[LIST(type, 0) & 3].nr_pages;
	u64 active = lv->lists[LIST(type, 1) & 3].nr_pages;
	u64 gb = (inactive + active) >> (30 - 12);
	u64 ratio = gb ? isqrt(10 * gb) : 1;

	return inactive * ratio < active;
}

/*
 * shrink_active_list(): move up to a batch from the active tail.
 * (Counters are bumped in memory, not in a loop-carried local: the verifier
 * cannot tell two iterations apart when a local counts 0, 1, 2, ...)
 */
static __always_inline void deactivate(struct te_lruvec *lv, int type)
{
	struct te_node __arena *n;
	u32 link;
	int i;

	bpf_for(i, 0, 32) {
		link = lv->lists[LIST(type, 1) & 3].tail;
		n = te_link(link);
		if (!n)
			break;
		te_push_head(lv, LIST(type, 0), link - 1, n);
		__sync_fetch_and_add(&nr_deactivated, 1);
	}
}

/*
 * Which type should this batch come from? Like get_scan_count(). Reads the
 * list sizes without the lock: it is a heuristic.
 */
static __always_inline int pick_type(struct tier_ext_victims_ctx *ctx,
				     struct te_lruvec *lv)
{
	u64 size[2], w[2], want_file, total;
	bool may[2];
	int t;

	for (t = 0; t < 2; t++) {
		size[t] = lv->lists[LIST(t, 0) & 3].nr_pages +
			  lv->lists[LIST(t, 1) & 3].nr_pages;
		may[t] = te_may(ctx, t) && size[t];
	}
	if (!may[0])
		return 1;
	if (!may[1])
		return 0;

	if (ctx->engine == TIER_EXT_ENGINE_CLASSIC &&
	    ctx->nr_scan_anon + ctx->nr_scan_file) {
		/* the kernel's own split, including its cost model */
		w[0] = ctx->nr_scan_anon;
		w[1] = ctx->nr_scan_file;
	} else {
		u64 swp = ctx->swappiness > 200 ? 200 : ctx->swappiness;

		w[0] = swp * size[0];
		w[1] = (200 - swp) * size[1];
	}
	if (!w[0])
		return 1;
	if (!w[1])
		return 0;

	/* keep this pass's nominations proportional to the weights */
	total = lv->x.picked[0] + lv->x.picked[1];
	want_file = total * w[1] / (w[0] + w[1]);
	return lv->x.picked[1] <= want_file;
}

/*
 * Nominate from the inactive tail of @type (refilled from the active tail
 * when it runs low or empty, like shrink_active_list()) until the request
 * is met. A global function: the verifier checks it once, on its own, which
 * keeps the locked loops cheap to verify. Returns the pages nominated.
 */
__noinline int pick_from(struct tier_ext_victims_ctx *ctx __arg_trusted,
			 int type)
{
	struct te_lruvec *lv;
	struct te_node __arena *n;
	u64 got = 0, pages, want;
	u32 link;
	int i;

	if (!ctx)
		return 0;
	lv = te_lv(ctx->nid);
	if (!lv)
		return 0;
	type &= 1;
	want = ctx->nr_requested;

	te_lock(lv);
	if (ctx->batch == 0)
		lv->x.picked[0] = lv->x.picked[1] = 0;
	if (inactive_is_low(lv, type))
		deactivate(lv, type);
	bpf_for(i, 0, TIER_EXT_MAX_VICTIMS) {
		if (got >= want)
			break;
		link = lv->lists[LIST(type, 0) & 3].tail;
		n = te_link(link);
		if (!n) {
			link = lv->lists[LIST(type, 1) & 3].tail;
			n = te_link(link);
			if (!n)
				break;
			__sync_fetch_and_add(&nr_deactivated, 1);
		}
		pages = te_nominate(ctx, lv, link - 1, n);
		if (!pages)
			break;
		got += pages;
	}
	lv->x.picked[type] += got;
	te_unlock(lv);

	__sync_fetch_and_add(&nr_pick[type], got);
	return got;
}

TIER_EXT_OP(pick_victims, tier_ext_victims_ctx)
{
	struct te_lruvec *lv = te_lv(ctx->nid);

	if (!lv)
		return 0;
	pick_from(ctx, pick_type(ctx, lv));
	return 0;
}

SEC(".struct_ops.link")
struct tier_ext_ops lru = {
	.folio_added	  = (void *)folio_added,
	.folio_accessed	  = (void *)folio_accessed,
	.folio_referenced = (void *)folio_referenced,
	.folio_evicted	  = (void *)folio_evicted,
	.folio_removed	  = (void *)folio_removed,
	.folio_migrated	  = (void *)folio_migrated,
	.pick_victims	  = (void *)pick_victims,
	.name		  = "lru",
};
