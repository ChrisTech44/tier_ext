// SPDX-License-Identifier: GPL-2.0
/*
 * lfu: frequency-based protection, for page cache and (on MGLRU) anon and
 * mmap'd files.
 *
 * The kernel's LRUs know little about how often a folio was used: the
 * classic LRU remembers two accesses, MGLRU a few. This policy counts
 * accesses in a BPF hash map (keyed by pfn):
 *   - folio_mark_accessed() (read()/write() of page cache), and
 *   - folio_referenced: MGLRU found the folio's page table entries young
 *     (page table walk during aging, or look-around during eviction), which
 *     is how anon memory and mmap'd files are seen to be used;
 * and, when reclaim reaches a folio,
 *   - keeps it if it was accessed at least hot_threshold times (and halves
 *     the count, so protection decays unless the folio stays hot);
 *   - evicts an unmapped page cache folio immediately if it was used at
 *     most once (a streaming read touches each folio once);
 *   - otherwise leaves the decision to the kernel.
 * With anon = 0 anon folios are left alone. Entries are dropped when a folio
 * is evicted, freed or migrated; the LRU hash bounds the memory used.
 *
 * MGLRU walks page tables only when it needs a new generation, which can be
 * rare. So lfu uses pick_victims without nominating anything: it asks for an
 * aging pass (TIER_EXT_AGE) at most every age_interval_ms, which keeps the
 * page table access counts fresh. Victim selection stays with the kernel.
 */
#include "tier_ext.bpf.h"

char _license[] SEC("license") = "GPL";

volatile u32 hot_threshold = 4;
volatile u32 anon = 1;		/* also count and protect anon folios */
volatile u32 age_interval_ms = 200;	/* 0: never ask for aging */
u64 last_age_ns;

struct {
	__uint(type, BPF_MAP_TYPE_LRU_HASH);
	__uint(max_entries, 1 << 20);
	__type(key, u64);	/* pfn */
	__type(value, u32);	/* access count */
} freq SEC(".maps");

u64 nr_keep, nr_keep_anon, nr_keep_mapped, nr_evict_cold, nr_default;
u64 nr_pt_access, nr_age_requests;

static __always_inline bool tracked(const struct tier_ext_folio_ctx *ctx)
{
	return anon || !te_anon(ctx);
}

TIER_EXT_EVENT(folio_added)
{
	u64 pfn = ctx->pfn;
	u32 zero = 0;

	if (!tracked(ctx))
		return;
	/* A folio re-added after a failed reclaim keeps its count. */
	bpf_map_update_elem(&freq, &pfn, &zero, BPF_NOEXIST);
}

static __always_inline void count_access(struct tier_ext_folio_ctx *ctx)
{
	u64 pfn = ctx->pfn;
	u32 one = 1, *cnt;

	if (!tracked(ctx))
		return;
	cnt = bpf_map_lookup_elem(&freq, &pfn);
	if (cnt)
		__sync_fetch_and_add(cnt, 1);
	else
		bpf_map_update_elem(&freq, &pfn, &one, BPF_ANY);
}

TIER_EXT_EVENT(folio_accessed)
{
	count_access(ctx);
}

/* page table accesses, found by MGLRU's walks */
TIER_EXT_EVENT(folio_referenced)
{
	__sync_fetch_and_add(&nr_pt_access, 1);
	count_access(ctx);
}

TIER_EXT_EVENT(folio_removed)
{
	u64 pfn = ctx->pfn;

	bpf_map_delete_elem(&freq, &pfn);
}

/* the count follows the data to its new page frame */
TIER_EXT_EVENT(folio_migrated)
{
	u64 old = ctx->old_pfn, pfn = ctx->pfn;
	u32 *cnt = bpf_map_lookup_elem(&freq, &old), c;

	if (!cnt)
		return;
	c = *cnt;
	bpf_map_delete_elem(&freq, &old);
	bpf_map_update_elem(&freq, &pfn, &c, BPF_ANY);
}

TIER_EXT_EVENT(folio_evicted)
{
	u64 pfn = ctx->pfn;

	bpf_map_delete_elem(&freq, &pfn);
}

static __always_inline s32 decide(struct tier_ext_folio_ctx *ctx)
{
	u64 pfn = ctx->pfn;
	u32 *cnt;

	if (!tracked(ctx))
		return TIER_EXT_DEFAULT;

	cnt = bpf_map_lookup_elem(&freq, &pfn);
	if (!cnt) {
		__sync_fetch_and_add(&nr_default, 1);
		return TIER_EXT_DEFAULT;
	}
	if (*cnt >= hot_threshold) {
		*cnt /= 2;
		__sync_fetch_and_add(&nr_keep, 1);
		if (te_anon(ctx))
			__sync_fetch_and_add(&nr_keep_anon, 1);
		else if (ctx->folio_flags & TIER_EXT_F_MAPPED)
			__sync_fetch_and_add(&nr_keep_mapped, 1);
		return TIER_EXT_KEEP;
	}
	if (*cnt <= 1 && !te_anon(ctx) &&
	    !(ctx->folio_flags & TIER_EXT_F_MAPPED)) {
		__sync_fetch_and_add(&nr_evict_cold, 1);
		return TIER_EXT_RECLAIM;
	}
	__sync_fetch_and_add(&nr_default, 1);
	return TIER_EXT_DEFAULT;
}

TIER_EXT_OP(folio_scan, tier_ext_folio_ctx)
{
	return decide(ctx);
}

/* No victims, just page table aging on demand (see above). */
TIER_EXT_OP(pick_victims, tier_ext_victims_ctx)
{
	u64 now = bpf_ktime_get_ns();

	if (!(ctx->flags & TIER_EXT_VF_CAN_AGE) || !age_interval_ms)
		return 0;
	if (now - last_age_ns < (u64)age_interval_ms * 1000000)
		return 0;
	last_age_ns = now;
	__sync_fetch_and_add(&nr_age_requests, 1);
	return TIER_EXT_AGE;
}

SEC(".struct_ops.link")
struct tier_ext_ops lfu = {
	.folio_added	= (void *)folio_added,
	.folio_accessed	= (void *)folio_accessed,
	.folio_referenced = (void *)folio_referenced,
	.folio_evicted	= (void *)folio_evicted,
	.folio_removed	= (void *)folio_removed,
	.folio_migrated	= (void *)folio_migrated,
	.folio_scan	= (void *)folio_scan,
	.pick_victims	= (void *)pick_victims,
	.name		= "lfu",
};
