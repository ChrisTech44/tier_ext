// SPDX-License-Identifier: GPL-2.0
/*
 * pin: a deliberately hostile policy used by the tests. It answers KEEP for
 * every folio and SKIP for the lruvec, i.e. it tries to make the attached
 * lruvecs unreclaimable. The kernel must still make progress (it ignores
 * KEEP/SKIP at priority 0), so a cgroup under this policy that exceeds
 * memory.max must end up reclaiming or OOM-killing, never hang.
 */
#include "tier_ext.bpf.h"

char _license[] SEC("license") = "GPL";

/* 1: also SKIP the whole lruvec */
volatile u32 skip_lruvec = 0;

TIER_EXT_OP(lruvec_shrink, tier_ext_lruvec_ctx)
{
	return skip_lruvec ? TIER_EXT_SKIP : TIER_EXT_DEFAULT;
}

TIER_EXT_OP(folio_scan, tier_ext_folio_ctx)
{
	return TIER_EXT_KEEP;
}

TIER_EXT_OP(folio_reclaim, tier_ext_folio_ctx)
{
	return TIER_EXT_KEEP;
}

SEC(".struct_ops.link")
struct tier_ext_ops pin = {
	.lruvec_shrink	= (void *)lruvec_shrink,
	.folio_scan	= (void *)folio_scan,
	.folio_reclaim	= (void *)folio_reclaim,
	.name		= "pin",
};
