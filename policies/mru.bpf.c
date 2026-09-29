// SPDX-License-Identifier: GPL-2.0
/*
 * mru: evict the most recently used page cache first.
 *
 * The acid test for victim nomination: with candidate filtering alone the
 * kernel's scanner only ever presents the oldest folios, so "newest first"
 * cannot be expressed. Here the policy keeps its own recency list of file
 * folios per node and nominates from the recent end. A file read once that
 * does not fit keeps its *first* part in memory - the opposite of LRU.
 *
 * Anon memory is left to the default engine (MRU makes no sense for it).
 * Port of cache_ext's cache_ext_mru.bpf.c.
 */
#include "tier_ext.bpf.h"

#define TE_NR_LISTS 1
struct te_extra {};
#include "te_engine.bpf.h"

char _license[] SEC("license") = "GPL";

u64 nr_picked, nr_added, nr_returned;

TIER_EXT_EVENT(folio_added)
{
	struct te_node __arena *n;
	struct te_lruvec *lv;
	u64 pfn = ctx->pfn;

	if (te_anon(ctx))
		return;
	n = te_node(pfn);
	lv = te_lv(ctx->nid);
	if (!n || !lv)
		return;
	te_lock(lv);
	if (te_returned(ctx)) {
		/* the kernel could not take it now: least preferred end */
		n->order = te_order(ctx->nr_pages);
		te_push_tail(lv, 0, pfn, n);
	} else if (!n->list) {
		n->order = te_order(ctx->nr_pages);
		te_push_head(lv, 0, pfn, n);
	}
	te_unlock(lv);
	if (te_returned(ctx))
		__sync_fetch_and_add(&nr_returned, 1);
	else
		__sync_fetch_and_add(&nr_added, 1);
}

TIER_EXT_EVENT(folio_accessed)
{
	struct te_node __arena *n = te_node(ctx->pfn);
	struct te_lruvec *lv = te_lv(ctx->nid);

	if (te_anon(ctx) || !n || !lv)
		return;
	te_lock(lv);
	if (n->list)
		te_push_head(lv, 0, ctx->pfn, n);
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

TIER_EXT_OP(pick_victims, tier_ext_victims_ctx)
{
	struct te_lruvec *lv = te_lv(ctx->nid);
	struct te_node __arena *n;
	u64 got = 0, pages;
	u32 link, next;
	int i;

	if (!lv || !te_may(ctx, 1))
		return 0;
	te_lock(lv);
	link = lv->lists[0].head;	/* most recently used end */
	for (i = 0; i < TIER_EXT_MAX_VICTIMS && link && got < ctx->nr_requested; i++) {
		n = te_link(link);
		if (!n)
			break;
		next = n->next;
		pages = te_nominate(ctx, lv, link - 1, n);
		if (!pages)
			break;
		got += pages;
		link = next;
	}
	te_unlock(lv);
	__sync_fetch_and_add(&nr_picked, got);
	return 0;
}

/* Reclaim what we nominated even if it was referenced: MRU means MRU. */
TIER_EXT_OP(folio_reclaim, tier_ext_folio_ctx)
{
	if ((ctx->reclaim_flags & TIER_EXT_RC_NOMINATED) && !te_anon(ctx))
		return TIER_EXT_RECLAIM;
	return TIER_EXT_DEFAULT;
}

SEC(".struct_ops.link")
struct tier_ext_ops mru = {
	.folio_added	= (void *)folio_added,
	.folio_accessed	= (void *)folio_accessed,
	.folio_evicted	= (void *)folio_evicted,
	.folio_removed	= (void *)folio_removed,
	.folio_migrated	= (void *)folio_migrated,
	.pick_victims	= (void *)pick_victims,
	.folio_reclaim	= (void *)folio_reclaim,
	.name		= "mru",
};
