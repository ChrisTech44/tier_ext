// SPDX-License-Identifier: GPL-2.0
/*
 * s3fifo: S3-FIFO (Yang et al., SOSP '23), anon and file. Port of cache_ext's
 * cache_ext_s3fifo.bpf.c, which covered page cache only.
 *
 * Per type and node: a small FIFO S (10% of the pages), a main FIFO M, and
 * a ghost queue G of recently evicted data (a BPF LRU hash keyed by the
 * folio's data_key: file offset or swap slot, so anon works too).
 *
 *   insert	data found in G goes to M, everything else to S
 *   access	freq = min(freq + 1, 3)   (folio_mark_accessed(), young PTEs
 *		found by MGLRU walks, or the kernel's reference check
 *		activating a nominated folio)
 *   evict	while S holds >= 10%: S's oldest folio moves to M if it was
 *		accessed more than once, else is evicted (and remembered in
 *		G); otherwise M's oldest folio is evicted if freq == 0, else
 *		freq-- and reinserted
 *
 * Page cache folios nominated by S3-FIFO are reclaimed regardless of the
 * kernel's reference check (cache_ext did the same). Anon folios keep the
 * kernel's second chance: their accesses are mostly invisible to events.
 *
 * Large folios: one sequential read() pass can mark a 2 MiB folio accessed
 * more than once (a read spanning folios marks the later ones, the next read
 * may mark the same folio again). S3-FIFO promotes on freq > 1, so a scan
 * would flood M. Accesses to a folio less than dedup_ms apart therefore
 * count once (cache_ext ran on 4 KiB page cache folios and had no need).
 */
#include "tier_ext.bpf.h"

#define SMALL(t)	(((t) & 1) * 2)
#define MAIN(t)		(((t) & 1) * 2 + 1)

#define TE_NR_LISTS 4
struct te_extra {
	u64 picked[2];
};
#include "te_engine.bpf.h"

char _license[] SEC("license") = "GPL";

#define F_MAIN	0x1	/* node->flags: in M, not S */
#define FREQ_MAX 3	/* node->meta: freq in bits 0-1, last access (ms) above */
#define FREQ(m)	((m) & 3)

volatile u32 dedup_ms = 1;

volatile u32 small_pct = 10;	/* size of S, percent of S + M */

struct {
	__uint(type, BPF_MAP_TYPE_LRU_HASH);
	__uint(max_entries, 1 << 18);
	__type(key, u64);
	__type(value, u8);
} ghosts SEC(".maps");

u64 nr_added, nr_ghost_hits, nr_to_main, nr_reinserted, nr_pick_small, nr_pick_main;

TIER_EXT_EVENT(folio_added)
{
	struct te_node __arena *n = te_node(ctx->pfn);
	struct te_lruvec *lv = te_lv(ctx->nid);
	bool returned = te_returned(ctx), ghost = false;
	int type = te_type(ctx);
	u64 key = ctx->data_key;

	if (!n || !lv)
		return;
	if (!returned && key)
		ghost = !bpf_map_delete_elem(&ghosts, &key);

	te_lock(lv);
	if (ctx->folio_flags & TIER_EXT_F_UNEVICTABLE) {
		te_unlink(lv, n);
	} else if (returned) {
		/* the kernel kept it; activation means it was referenced */
		if ((ctx->folio_flags & TIER_EXT_F_ACTIVE) && FREQ(n->meta) < FREQ_MAX)
			n->meta++;
		te_push_head(lv, (n->flags & F_MAIN) ? MAIN(type) : SMALL(type),
			     ctx->pfn, n);
	} else if (!n->list) {
		n->order = te_order(ctx->nr_pages);
		n->meta = 0;
		n->flags = ghost ? F_MAIN : 0;
		te_push_head(lv, ghost ? MAIN(type) : SMALL(type), ctx->pfn, n);
	}
	te_unlock(lv);

	if (!returned)
		__sync_fetch_and_add(&nr_added, 1);
	if (ghost)
		__sync_fetch_and_add(&nr_ghost_hits, 1);
}

static __always_inline void accessed(struct tier_ext_folio_ctx *ctx)
{
	struct te_node __arena *n = te_node(ctx->pfn);
	struct te_lruvec *lv = te_lv(ctx->nid);
	u32 now = (bpf_ktime_get_ns() / 1000000) & 0x3fffffff, last;

	if (!n || !lv)
		return;
	te_lock(lv);
	last = n->meta >> 2;
	if (n->list && ((now - last) & 0x3fffffff) >= dedup_ms) {
		n->meta = (now << 2) |
			  (FREQ(n->meta) < FREQ_MAX ? FREQ(n->meta) + 1 : FREQ_MAX);
	}
	te_unlock(lv);
}

TIER_EXT_EVENT(folio_accessed)
{
	accessed(ctx);
}

TIER_EXT_EVENT(folio_referenced)
{
	accessed(ctx);
}

TIER_EXT_EVENT(folio_evicted)
{
	struct te_node __arena *n = te_node(ctx->pfn);
	struct te_lruvec *lv = te_lv(ctx->nid);
	u64 key = ctx->data_key;
	bool from_small = false;
	u8 one = 1;

	if (!n || !lv)
		return;
	te_lock(lv);
	from_small = !(n->flags & F_MAIN);
	te_unlink(lv, n);
	n->flags = 0;
	te_unlock(lv);
	/* G remembers what S evicted */
	if (from_small && key)
		bpf_map_update_elem(&ghosts, &key, &one, BPF_ANY);
}

TIER_EXT_EVENT(folio_removed)
{
	te_forget(ctx->nid, ctx->pfn);
}

TIER_EXT_EVENT(folio_migrated)
{
	te_migrate(ctx);
}

/* Evict from S: promote folios accessed more than once to M. */
__noinline int evict_small(struct tier_ext_victims_ctx *ctx __arg_trusted,
			   int type)
{
	struct te_lruvec *lv;
	struct te_node __arena *n;
	u64 got = 0, pages;
	u32 link;
	int i;

	if (!ctx)
		return 0;
	lv = te_lv(ctx->nid);
	if (!lv)
		return 0;
	type &= 1;
	te_lock(lv);
	bpf_for(i, 0, 16 * TIER_EXT_MAX_VICTIMS) {
		if (got >= ctx->nr_requested)
			break;
		link = lv->lists[SMALL(type)].tail;
		n = te_link(link);
		if (!n)
			break;
		if (FREQ(n->meta) > 1) {
			n->flags |= F_MAIN;
			te_push_head(lv, MAIN(type), link - 1, n);
			__sync_fetch_and_add(&nr_to_main, 1);
			continue;
		}
		pages = te_nominate(ctx, lv, link - 1, n);
		if (!pages)
			break;
		got += pages;
	}
	lv->x.picked[type] += got;
	te_unlock(lv);
	__sync_fetch_and_add(&nr_pick_small, got);
	return got;
}

/* Evict from M: second chance by frequency. */
__noinline int evict_main(struct tier_ext_victims_ctx *ctx __arg_trusted,
			  int type)
{
	struct te_lruvec *lv;
	struct te_node __arena *n;
	u64 got = 0, pages;
	u32 link;
	int i;

	if (!ctx)
		return 0;
	lv = te_lv(ctx->nid);
	if (!lv)
		return 0;
	type &= 1;
	te_lock(lv);
	bpf_for(i, 0, 16 * TIER_EXT_MAX_VICTIMS) {
		if (got >= ctx->nr_requested)
			break;
		link = lv->lists[MAIN(type)].tail;
		n = te_link(link);
		if (!n)
			break;
		if (FREQ(n->meta)) {
			n->meta--;
			te_push_head(lv, MAIN(type), link - 1, n);
			__sync_fetch_and_add(&nr_reinserted, 1);
			continue;
		}
		pages = te_nominate(ctx, lv, link - 1, n);
		if (!pages)
			break;
		got += pages;
	}
	lv->x.picked[type] += got;
	te_unlock(lv);
	__sync_fetch_and_add(&nr_pick_main, got);
	return got;
}

TIER_EXT_OP(pick_victims, tier_ext_victims_ctx)
{
	struct te_lruvec *lv = te_lv(ctx->nid);
	u64 size[2], small;
	int type, got, from_small, q;

	if (!lv)
		return 0;
	if (ctx->batch == 0)
		lv->x.picked[0] = lv->x.picked[1] = 0;
	/* unlocked reads: sizing heuristics */
	size[0] = lv->lists[SMALL(0)].nr_pages + lv->lists[MAIN(0)].nr_pages;
	size[1] = lv->lists[SMALL(1)].nr_pages + lv->lists[MAIN(1)].nr_pages;
	type = te_pick_type(ctx, size[0], size[1], lv->x.picked[0],
			    lv->x.picked[1]) & 1;
	small = lv->lists[SMALL(type)].nr_pages;
	from_small = small * 100 >= size[type] * small_pct;
	got = from_small ? evict_small(ctx, type) : evict_main(ctx, type);
	if (got >= ctx->nr_requested)
		return 0;
	/*
	 * Short. If the queue still has folios, it spent this call moving
	 * them (S -> M, or M's second chances): continue in the next call,
	 * as S3-FIFO would, rather than evicting from the other queue or
	 * leaving the rest to the kernel's (stale) ordering.
	 */
	q = from_small ? SMALL(type) : MAIN(type);
	if (lv->lists[q & 3].nr)
		return TIER_EXT_AGAIN;
	if (from_small)
		evict_main(ctx, type);
	else
		evict_small(ctx, type);
	return 0;
}

TIER_EXT_OP(folio_reclaim, tier_ext_folio_ctx)
{
	if ((ctx->reclaim_flags & TIER_EXT_RC_NOMINATED) && !te_anon(ctx))
		return TIER_EXT_RECLAIM;
	return TIER_EXT_DEFAULT;
}

SEC(".struct_ops.link")
struct tier_ext_ops s3fifo = {
	.folio_added	  = (void *)folio_added,
	.folio_accessed	  = (void *)folio_accessed,
	.folio_referenced = (void *)folio_referenced,
	.folio_evicted	  = (void *)folio_evicted,
	.folio_removed	  = (void *)folio_removed,
	.folio_migrated	  = (void *)folio_migrated,
	.pick_victims	  = (void *)pick_victims,
	.folio_reclaim	  = (void *)folio_reclaim,
	.name		  = "s3fifo",
};
