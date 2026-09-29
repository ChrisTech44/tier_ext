// SPDX-License-Identifier: GPL-2.0
/*
 * fifo: evict in insertion order, anon and file. Port of cache_ext's
 * cache_ext_fifo.bpf.c (which covered page cache only).
 *
 * One FIFO per type and node. Accesses do not matter; nominated folios are
 * reclaimed even if referenced (folio_reclaim), as cache_ext did by
 * reclaiming with reclaim_pages(). The anon/file split follows the kernel
 * (te_pick_type()).
 */
#include "tier_ext.bpf.h"

#define TE_NR_LISTS 2
struct te_extra {
	u64 picked[2];
};
#include "te_engine.bpf.h"

char _license[] SEC("license") = "GPL";

u64 nr_added, nr_returned, nr_pick[2];

TIER_EXT_EVENT(folio_added)
{
	struct te_node __arena *n = te_node(ctx->pfn);
	struct te_lruvec *lv = te_lv(ctx->nid);

	if (!n || !lv)
		return;
	te_lock(lv);
	if (ctx->folio_flags & TIER_EXT_F_UNEVICTABLE) {
		te_unlink(lv, n);
	} else if (te_returned(ctx) || !n->list) {
		n->order = te_order(ctx->nr_pages);
		te_push_head(lv, te_type(ctx), ctx->pfn, n);
	}
	te_unlock(lv);
	__sync_fetch_and_add(te_returned(ctx) ? &nr_returned : &nr_added, 1);
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

/* Global: the verifier checks the locked loop once. */
__noinline int pick(struct tier_ext_victims_ctx *ctx __arg_trusted)
{
	struct te_lruvec *lv;
	struct te_node __arena *n;
	u64 got = 0, pages;
	u32 link;
	int type, i;

	if (!ctx)
		return 0;
	lv = te_lv(ctx->nid);
	if (!lv)
		return 0;
	te_lock(lv);
	if (ctx->batch == 0)
		lv->x.picked[0] = lv->x.picked[1] = 0;
	type = te_pick_type(ctx, lv->lists[0].nr_pages, lv->lists[1].nr_pages,
			    lv->x.picked[0], lv->x.picked[1]) & 1;
	bpf_for(i, 0, TIER_EXT_MAX_VICTIMS) {
		if (got >= ctx->nr_requested)
			break;
		link = lv->lists[type].tail;
		n = te_link(link);
		if (!n)
			break;
		pages = te_nominate(ctx, lv, link - 1, n);
		if (!pages)
			break;
		got += pages;
	}
	lv->x.picked[type] += got;
	te_unlock(lv);
	__sync_fetch_and_add(&nr_pick[type], got);
	return 0;
}

TIER_EXT_OP(pick_victims, tier_ext_victims_ctx)
{
	return pick(ctx);
}

/* FIFO means FIFO: ignore references of the folios we nominated. */
TIER_EXT_OP(folio_reclaim, tier_ext_folio_ctx)
{
	if (ctx->reclaim_flags & TIER_EXT_RC_NOMINATED)
		return TIER_EXT_RECLAIM;
	return TIER_EXT_DEFAULT;
}

SEC(".struct_ops.link")
struct tier_ext_ops fifo = {
	.folio_added	= (void *)folio_added,
	.folio_evicted	= (void *)folio_evicted,
	.folio_removed	= (void *)folio_removed,
	.folio_migrated	= (void *)folio_migrated,
	.pick_victims	= (void *)pick_victims,
	.folio_reclaim	= (void *)folio_reclaim,
	.name		= "fifo",
};
