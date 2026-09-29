// SPDX-License-Identifier: GPL-2.0
/*
 * tier_demote: choose between demotion and eviction per folio type.
 *
 * Default kernel behaviour on a tiered machine demotes every reclaimable folio
 * from a fast node to the next slower node when demotion is enabled. This
 * policy, attached to a fast-node lruvec, instead
 *   - drops clean page cache directly (re-reading it is cheap and it would
 *     only pollute the slow tier), and
 *   - demotes anon only, never swapping it from the fast tier: if demotion
 *     is impossible the anon folio stays where it is.
 * Both halves can be switched off with --set file_no_demote=0 / anon_demote_only=0.
 */
#include "tier_ext.bpf.h"

char _license[] SEC("license") = "GPL";

volatile u32 file_no_demote = 1;
volatile u32 anon_demote_only = 1;

TIER_EXT_OP(folio_reclaim, tier_ext_folio_ctx)
{
	if (te_anon(ctx))
		return anon_demote_only ? TIER_EXT_DEFAULT | TIER_EXT_DEMOTE_ONLY :
					  TIER_EXT_DEFAULT;

	if (file_no_demote && !(ctx->folio_flags & TIER_EXT_F_DIRTY))
		return TIER_EXT_DEFAULT | TIER_EXT_NO_DEMOTE;
	return TIER_EXT_DEFAULT;
}

SEC(".struct_ops.link")
struct tier_ext_ops tier_demote = {
	.folio_reclaim	= (void *)folio_reclaim,
	.name		= "tier_demote",
};
