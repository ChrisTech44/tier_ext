// SPDX-License-Identifier: GPL-2.0
/*
 * garbage: a hostile pick_victims, for testing the kernel's validation.
 *
 * Nominates pfns that are not this lruvec's folios: random numbers (most
 * beyond the end of memory), low pfns (kernel memory, reserved pages), and a
 * sweep over the whole pfn range, which hits free pages, tail pages, page
 * tables, slab and other cgroups' folios. None of that may be reclaimed or
 * crash the kernel; the default engine must still make progress.
 *
 *   mode = 0: mix of everything (default)
 *   mode = 1: sweep only (maximises other cgroups' folios)
 */
#include "tier_ext.bpf.h"

char _license[] SEC("license") = "GPL";

volatile u32 mode;
volatile u64 max_pfn = 1 << 22;	/* set by the test from /proc/zoneinfo */
u64 cursor, nr_calls, nr_nominated;

TIER_EXT_OP(pick_victims, tier_ext_victims_ctx)
{
	u64 pfn;
	int i;

	__sync_fetch_and_add(&nr_calls, 1);
	for (i = 0; i < TIER_EXT_MAX_VICTIMS; i++) {
		u32 r = bpf_get_prandom_u32();

		if (mode == 1 || (r & 3) == 0) {
			pfn = __sync_fetch_and_add(&cursor, 97) % (max_pfn ?: 1);
		} else if ((r & 3) == 1) {
			pfn = ((u64)bpf_get_prandom_u32() << 32) | r;
		} else if ((r & 3) == 2) {
			pfn = r & 0xffff;		/* low memory */
		} else {
			pfn = (u64)r % (max_pfn ?: 1);
		}
		if (bpf_tier_ext_add_victim(ctx, pfn))
			break;
		__sync_fetch_and_add(&nr_nominated, 1);
	}
	/* ask for aging too: must be harmless */
	return i & 1 ? TIER_EXT_AGE : 0;
}

SEC(".struct_ops.link")
struct tier_ext_ops garbage = {
	.pick_victims	= (void *)pick_victims,
	.name		= "garbage",
};
