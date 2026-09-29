// SPDX-License-Identifier: GPL-2.0
/*
 * sampling: LFU by sampling, page cache. Port of cache_ext's
 * cache_ext_sampling.bpf.c.
 *
 * Every folio_mark_accessed() counts. To choose a victim, SAMPLE folios are
 * taken from the old end of the list; the least frequently used one is
 * nominated and the others move to the new end (bpf_cache_ext_list_sample()
 * semantics). Anon memory is left to the kernel's engine.
 */
#include "tier_ext.bpf.h"

#define TE_NR_LISTS 1
struct te_extra {};
#include "te_engine.bpf.h"

char _license[] SEC("license") = "GPL";

#define SAMPLE 20

u64 nr_added, nr_picked, nr_sampled;

TIER_EXT_EVENT(folio_added)
{
	struct te_node __arena *n = te_node(ctx->pfn);
	struct te_lruvec *lv = te_lv(ctx->nid);

	if (te_anon(ctx) || !n || !lv)
		return;
	te_lock(lv);
	if (te_returned(ctx)) {
		te_push_head(lv, 0, ctx->pfn, n);
	} else if (!n->list) {
		n->order = te_order(ctx->nr_pages);
		n->meta = 1;
		te_push_head(lv, 0, ctx->pfn, n);
	}
	te_unlock(lv);
	__sync_fetch_and_add(&nr_added, 1);
}

TIER_EXT_EVENT(folio_accessed)
{
	struct te_node __arena *n = te_node(ctx->pfn);
	struct te_lruvec *lv = te_lv(ctx->nid);

	if (te_anon(ctx) || !n || !lv)
		return;
	te_lock(lv);
	if (n->list && n->meta < 0xffffffff)
		n->meta++;
	te_unlock(lv);
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

__noinline int pick(struct tier_ext_victims_ctx *ctx __arg_trusted)
{
	struct te_lruvec *lv;
	struct te_node __arena *n, *bn;
	u64 got = 0, pages, score, best_score = ~0ULL;
	u32 link, best = 0;
	int i;

	if (!ctx)
		return 0;
	lv = te_lv(ctx->nid);
	if (!lv)
		return 0;
	te_lock(lv);
	bpf_for(i, 0, SAMPLE * TIER_EXT_MAX_VICTIMS) {
		if (got >= ctx->nr_requested)
			break;
		link = lv->lists[0].tail;
		n = te_link(link);
		if (!n)
			break;
		score = n->meta;
		te_unlink(lv, n);
		if (!best || score < best_score) {
			bn = te_link(best);
			if (bn)
				te_push_head(lv, 0, best - 1, bn);
			best = link;
			best_score = score;
		} else {
			te_push_head(lv, 0, link - 1, n);
		}
		if (i % SAMPLE == SAMPLE - 1) {
			bn = te_link(best);
			if (bn) {
				pages = te_nominate(ctx, lv, best - 1, bn);
				if (!pages) {
					te_push_head(lv, 0, best - 1, bn);
					best = 0;
					break;
				}
				got += pages;
			}
			best = 0;
		}
	}
	/* a partial sample (the list was short): take its best */
	bn = te_link(best);
	if (bn) {
		pages = te_nominate(ctx, lv, best - 1, bn);
		if (pages)
			got += pages;
		else
			te_push_head(lv, 0, best - 1, bn);
	}
	te_unlock(lv);
	__sync_fetch_and_add(&nr_picked, got);
	return 0;
}

TIER_EXT_OP(pick_victims, tier_ext_victims_ctx)
{
	if (!te_may(ctx, 1))
		return 0;
	return pick(ctx);
}

TIER_EXT_OP(folio_reclaim, tier_ext_folio_ctx)
{
	if ((ctx->reclaim_flags & TIER_EXT_RC_NOMINATED) && !te_anon(ctx))
		return TIER_EXT_RECLAIM;
	return TIER_EXT_DEFAULT;
}

SEC(".struct_ops.link")
struct tier_ext_ops sampling = {
	.folio_added	= (void *)folio_added,
	.folio_accessed	= (void *)folio_accessed,
	.folio_evicted	= (void *)folio_evicted,
	.folio_removed	= (void *)folio_removed,
	.folio_migrated	= (void *)folio_migrated,
	.pick_victims	= (void *)pick_victims,
	.folio_reclaim	= (void *)folio_reclaim,
	.name		= "sampling",
};
