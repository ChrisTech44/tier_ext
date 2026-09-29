/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Common include for tier_ext BPF policies.
 *
 * vmlinux.h (generated from the tier_ext kernel) provides struct tier_ext_ops,
 * the two context structures and the TIER_EXT_* enums.
 */
#ifndef __TIER_EXT_BPF_H
#define __TIER_EXT_BPF_H

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

/*
 * Declare a struct_ops callback whose body sees its argument as "ctx":
 *
 *	TIER_EXT_OP(folio_scan, tier_ext_folio_ctx) { return ...ctx->pfn...; }
 *	TIER_EXT_EVENT(folio_added) { ... }
 *
 * (BPF_PROG() itself reserves the name ctx, hence the inner function.)
 */
#define TIER_EXT_OP(op, ctx_t)						\
	static __always_inline s32 ____te_##op(struct ctx_t *ctx);	\
	SEC("struct_ops/" #op)						\
	s32 BPF_PROG(op, struct ctx_t *__arg)				\
	{								\
		return ____te_##op(__arg);				\
	}								\
	static __always_inline s32 ____te_##op(struct ctx_t *ctx)

#define TIER_EXT_EVENT(op)						\
	static __always_inline void ____te_##op(struct tier_ext_folio_ctx *ctx); \
	SEC("struct_ops/" #op)						\
	void BPF_PROG(op, struct tier_ext_folio_ctx *__arg)		\
	{								\
		____te_##op(__arg);					\
	}								\
	static __always_inline void ____te_##op(struct tier_ext_folio_ctx *ctx)

static __always_inline bool te_anon(const struct tier_ext_folio_ctx *ctx)
{
	return ctx->folio_flags & TIER_EXT_F_ANON;
}

/* Page-count helpers use u64 counters in a per-node array. */
#define TE_MAX_NODES 64

/* #defines of linux/tier_ext.h, which BTF does not carry */
#define TIER_EXT_MAX_VICTIMS	32
#define TIER_EXT_KEY_SWAP	(1ULL << 63)

#endif /* __TIER_EXT_BPF_H */
