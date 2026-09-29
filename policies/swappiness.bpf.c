// SPDX-License-Identifier: GPL-2.0
/*
 * swappiness: give one (memcg, node) its own anon/file balance.
 *
 * cgroup v2 has no per-cgroup swappiness, and no knob at all is per node.
 * With this policy, e.g. value=0 makes reclaim of the attached lruvecs drop
 * page cache only, while value=200 makes it prefer anon (swap / demotion).
 */
#include "tier_ext.bpf.h"

char _license[] SEC("license") = "GPL";

/* 0..200, set by the loader (--set value=N). */
volatile s32 value = 0;

TIER_EXT_OP(swappiness, tier_ext_lruvec_ctx)
{
	return value;
}

SEC(".struct_ops.link")
struct tier_ext_ops swappiness_policy = {
	.swappiness	= (void *)swappiness,
	.name		= "swappiness",
};
