// SPDX-License-Identifier: GPL-2.0
/*
 * observe: a policy that never changes a decision. It counts every callback
 * per node so you can see which lruvecs tier_ext is consulted for, and checks
 * that the kernel only ever calls it for the cgroup/nodes it was attached to.
 */
#include "tier_ext.bpf.h"

char _license[] SEC("license") = "GPL";

/*
 * Per-node counters, indexed by NUMA node. The loader prints the non-zero
 * entries on exit, e.g. "scan[1] = 4096".
 */
u64 shrink[TE_MAX_NODES], swappiness_calls[TE_MAX_NODES];
u64 scan[TE_MAX_NODES], scan_mglru[TE_MAX_NODES], scan_classic[TE_MAX_NODES];
u64 reclaim[TE_MAX_NODES], added[TE_MAX_NODES], accessed[TE_MAX_NODES];
u64 evicted[TE_MAX_NODES];
/* Calls for a cgroup other than expect_cgroup: must stay 0. */
u64 wrong_target;

/* Set by the loader to the cgroup id the policy is attached to. */
volatile u64 expect_cgroup = 0;

#define COUNT(arr, nid, cg)						\
	do {								\
		u32 __n = (nid);					\
		if (expect_cgroup && (cg) != expect_cgroup)		\
			__sync_fetch_and_add(&wrong_target, 1);		\
		if (__n < TE_MAX_NODES)					\
			__sync_fetch_and_add(&arr[__n], 1);		\
	} while (0)

TIER_EXT_OP(lruvec_shrink, tier_ext_lruvec_ctx)
{
	COUNT(shrink, ctx->nid, ctx->cgroup_id);
	return TIER_EXT_DEFAULT;
}

TIER_EXT_OP(swappiness, tier_ext_lruvec_ctx)
{
	COUNT(swappiness_calls, ctx->nid, ctx->cgroup_id);
	return -1;
}

TIER_EXT_OP(folio_scan, tier_ext_folio_ctx)
{
	COUNT(scan, ctx->nid, ctx->cgroup_id);
	if (ctx->engine == TIER_EXT_ENGINE_MGLRU)
		COUNT(scan_mglru, ctx->nid, ctx->cgroup_id);
	else
		COUNT(scan_classic, ctx->nid, ctx->cgroup_id);
	return TIER_EXT_DEFAULT;
}

TIER_EXT_OP(folio_reclaim, tier_ext_folio_ctx)
{
	COUNT(reclaim, ctx->nid, ctx->cgroup_id);
	return TIER_EXT_DEFAULT;
}

TIER_EXT_EVENT(folio_added)
{
	COUNT(added, ctx->nid, ctx->cgroup_id);
}

TIER_EXT_EVENT(folio_accessed)
{
	COUNT(accessed, ctx->nid, ctx->cgroup_id);
}

TIER_EXT_EVENT(folio_evicted)
{
	COUNT(evicted, ctx->nid, ctx->cgroup_id);
}

SEC(".struct_ops.link")
struct tier_ext_ops observe = {
	.lruvec_shrink	= (void *)lruvec_shrink,
	.swappiness	= (void *)swappiness,
	.folio_scan	= (void *)folio_scan,
	.folio_reclaim	= (void *)folio_reclaim,
	.folio_added	= (void *)folio_added,
	.folio_accessed	= (void *)folio_accessed,
	.folio_evicted	= (void *)folio_evicted,
	.name		= "observe",
};
