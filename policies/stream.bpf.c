// SPDX-License-Identifier: GPL-2.0
/*
 * stream: for cgroups that stream through file data (backups, scans, ETL).
 *
 * Page cache of the attached lruvecs is evicted as soon as reclaim looks at
 * it, even if it was accessed again: the MGLRU tier protection and the
 * classic "referenced twice -> activate" rule are both bypassed. Anon memory
 * keeps the default policy. The effect is that a streaming cgroup recycles
 * its own cache instead of growing it at the expense of everyone else.
 */
#include "tier_ext.bpf.h"

char _license[] SEC("license") = "GPL";

TIER_EXT_OP(folio_scan, tier_ext_folio_ctx)
{
	return te_anon(ctx) ? TIER_EXT_DEFAULT : TIER_EXT_RECLAIM;
}

TIER_EXT_OP(folio_reclaim, tier_ext_folio_ctx)
{
	return te_anon(ctx) ? TIER_EXT_DEFAULT : TIER_EXT_RECLAIM;
}

SEC(".struct_ops.link")
struct tier_ext_ops stream = {
	.folio_scan	= (void *)folio_scan,
	.folio_reclaim	= (void *)folio_reclaim,
	.name		= "stream",
};
