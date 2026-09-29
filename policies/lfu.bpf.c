// SPDX-License-Identifier: GPL-2.0
/*
 * lfu: frequency-based protection for page cache.
 *
 * The kernel's LRUs know little about how often a file folio was read: the
 * classic LRU remembers two accesses, MGLRU a few. This policy counts every
 * folio_mark_accessed() in a BPF hash map (keyed by pfn) and, when reclaim
 * reaches a folio,
 *   - keeps it if it was accessed at least hot_threshold times (and halves
 *     the count, so protection decays unless the folio stays hot);
 *   - evicts it immediately if it was never accessed since it was added;
 *   - otherwise leaves the decision to the kernel.
 * Anon folios are not affected. Entries are dropped on eviction; the LRU hash
 * bounds the memory used if some are missed (e.g. folios freed by truncation).
 */
#include "tier_ext.bpf.h"

char _license[] SEC("license") = "GPL";

volatile u32 hot_threshold = 4;

struct {
	__uint(type, BPF_MAP_TYPE_LRU_HASH);
	__uint(max_entries, 1 << 20);
	__type(key, u64);	/* pfn */
	__type(value, u32);	/* access count */
} freq SEC(".maps");

u64 nr_keep, nr_evict_cold, nr_default;

TIER_EXT_EVENT(folio_added)
{
	u64 pfn = ctx->pfn;
	u32 zero = 0;

	if (te_anon(ctx))
		return;
	/* A folio re-added after a failed reclaim keeps its count. */
	bpf_map_update_elem(&freq, &pfn, &zero, BPF_NOEXIST);
}

TIER_EXT_EVENT(folio_accessed)
{
	u64 pfn = ctx->pfn;
	u32 one = 1, *cnt;

	if (te_anon(ctx))
		return;
	cnt = bpf_map_lookup_elem(&freq, &pfn);
	if (cnt)
		__sync_fetch_and_add(cnt, 1);
	else
		bpf_map_update_elem(&freq, &pfn, &one, BPF_ANY);
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

	if (te_anon(ctx))
		return TIER_EXT_DEFAULT;

	cnt = bpf_map_lookup_elem(&freq, &pfn);
	if (!cnt) {
		__sync_fetch_and_add(&nr_default, 1);
		return TIER_EXT_DEFAULT;
	}
	if (*cnt >= hot_threshold) {
		*cnt /= 2;
		__sync_fetch_and_add(&nr_keep, 1);
		return TIER_EXT_KEEP;
	}
	if (*cnt == 0 && !(ctx->folio_flags & TIER_EXT_F_MAPPED)) {
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

SEC(".struct_ops.link")
struct tier_ext_ops lfu = {
	.folio_added	= (void *)folio_added,
	.folio_accessed	= (void *)folio_accessed,
	.folio_evicted	= (void *)folio_evicted,
	.folio_scan	= (void *)folio_scan,
	.name		= "lfu",
};
