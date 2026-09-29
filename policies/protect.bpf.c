// SPDX-License-Identifier: GPL-2.0
/*
 * protect: a per-node memory.low. Reclaim skips the attached lruvecs while
 * they hold fewer than min_pages pages. Unlike memory.low it can differ per
 * NUMA node, e.g. protect a cgroup's DRAM footprint but not its slow-tier one.
 *
 * The kernel ignores SKIP at reclaim priority 0, so this can delay reclaim of
 * the protected lruvecs but cannot cause an OOM by itself.
 */
#include "tier_ext.bpf.h"

char _license[] SEC("license") = "GPL";

volatile u64 min_pages = 0;

u64 skipped = 0;

TIER_EXT_OP(lruvec_shrink, tier_ext_lruvec_ctx)
{
	if (ctx->nr_anon + ctx->nr_file <= min_pages) {
		__sync_fetch_and_add(&skipped, 1);
		return TIER_EXT_SKIP;
	}
	return TIER_EXT_DEFAULT;
}

SEC(".struct_ops.link")
struct tier_ext_ops protect = {
	.lruvec_shrink	= (void *)lruvec_shrink,
	.name		= "protect",
};
