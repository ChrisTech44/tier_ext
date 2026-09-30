// SPDX-License-Identifier: GPL-2.0
/*
 * mglru: the multi-generational LRU, re-implemented as a tier_ext engine for
 * page cache *and* anon memory.
 *
 * A port of mm/vmscan.c's MGLRU to BPF, per lruvec, with its own state:
 *
 *   generations	max_seq, min_seq[anon/file]; 4 generations per type,
 *			one te_engine list each (8 lists per node). New
 *			folios are placed like lru_gen_folio_seq() does.
 *   tiers		per-folio access count (folio_mark_accessed(): refs
 *			up to 4, then workingset), tier = order_base_2(refs),
 *			workingset = last tier.
 *   aging		when the oldest evictable generation gets too close
 *			to max_seq (should_run_aging()), max_seq is bumped and
 *			TIER_EXT_AGE asks the kernel for a page table walk:
 *			young folios come back as folio_referenced and move
 *			to the youngest generation (folio_update_gen()).
 *   eviction		from the oldest generation of the type chosen by the
 *			refault feedback loop (get_type_to_scan()), protecting
 *			tiers whose refault rate is higher than tier 0's
 *			(get_tier_idx(), sort_folio()).
 *   refaults		a ghost map keyed by the folio's data_key (file
 *			offset or swap slot) remembers the tier of evicted
 *			folios, which gives the PID controller its refault
 *			counts - for anon as well as for file.
 *
 * Differences from the kernel are listed in design/engines.md.
 */
#include "tier_ext.bpf.h"

#define NR_GENS		4	/* MAX_NR_GENS */
#define MIN_GENS	2	/* MIN_NR_GENS */
#define NR_TIERS	4	/* MAX_NR_TIERS */
#define REFS_MAX	4	/* folio_lru_refs() with all LRU_REFS bits set */
#define LRU_BATCH	64	/* MIN_LRU_BATCH */
#define MAX_SWP		200
#define ANON_ONLY	201	/* SWAPPINESS_ANON_ONLY */
#define DEF_PRIO	12

#define TE_NR_LISTS	(2 * NR_GENS)
#define LIST(type, seq)	((((type) & 1) * NR_GENS + ((seq) % NR_GENS)) & 7)

struct te_extra {
	u64 max_seq;
	u64 min_seq[2];
	/* the PID controller (NR_HIST_GENS == 1) */
	u64 avg_refaulted[2][NR_TIERS];
	u64 avg_total[2][NR_TIERS];
	u64 refaulted[2][NR_TIERS];
	u64 evicted[2][NR_TIERS];
	u64 protected[2][NR_TIERS];
	u32 init;
	u32 __pad;
};

#include "te_engine.bpf.h"

char _license[] SEC("license") = "GPL";

/* node->meta: folio_lru_refs() (0..REFS_MAX); node->flags: */
#define F_WS		0x1	/* PG_workingset */
#define F_OWN		0x2	/* inserted by us; refs/flags are meaningful */

/* Ghost entries: the tier and generation of evicted folios. */
struct ghost {
	u64 token;	/* min_seq[type] at eviction */
	u8 refs;
	u8 ws;
	u8 type;
	u8 __pad[5];
};

struct {
	__uint(type, BPF_MAP_TYPE_LRU_HASH);
	__uint(max_entries, 1 << 18);
	__type(key, u64);
	__type(value, struct ghost);
} ghosts SEC(".maps");

u64 nr_added, nr_returned, nr_refaults, nr_aged, nr_age_requests;
u64 nr_protected, nr_promoted, nr_ws_set;
u64 nr_pick[2];

/* ------------------------------------------------------------- helpers */

/* lru_tier_from_refs() */
static __always_inline int tier_of(u32 refs, bool ws)
{
	if (ws)
		return NR_TIERS - 1;
	if (refs <= 1)
		return 0;
	if (refs == 2)
		return 1;
	return 2;	/* order_base_2(3 or 4) */
}

static __always_inline void lv_init(struct te_lruvec *lv)
{
	if (lv->x.init)
		return;
	lv->x.max_seq = MIN_GENS + 1;
	lv->x.min_seq[0] = lv->x.min_seq[1] = 0;
	lv->x.init = 1;
}

static __always_inline u64 nr_gens(struct te_lruvec *lv, int type)
{
	return lv->x.max_seq - lv->x.min_seq[type & 1] + 1;
}

/* Swappiness as MGLRU sees it: 0 = file only, ANON_ONLY = anon only. */
static __always_inline int eff_swappiness(struct tier_ext_victims_ctx *ctx)
{
	if (!te_may(ctx, 0))
		return 0;
	if (!te_may(ctx, 1))
		return ANON_ONLY;
	return ctx->swappiness > MAX_SWP ? ANON_ONLY : ctx->swappiness;
}

static __always_inline int min_type(int swp)
{
	return swp == ANON_ONLY ? 0 : !swp;	/* LRU_GEN_ANON or LRU_GEN_FILE */
}

static __always_inline int max_type(int swp)
{
	return swp == ANON_ONLY ? 0 : 1;
}

static __always_inline u64 evictable_min_seq(struct te_lruvec *lv, int swp)
{
	u64 a = lv->x.min_seq[min_type(swp) & 1];
	u64 b = lv->x.min_seq[max_type(swp) & 1];

	return a < b ? a : b;
}

/* reset_ctrl_pos(carryover = true) with NR_HIST_GENS == 1 */
static __always_inline void carryover(struct te_lruvec *lv, int type)
{
	int t;

	type &= 1;
	for (t = 0; t < NR_TIERS; t++) {
		lv->x.avg_refaulted[type][t] =
			(lv->x.avg_refaulted[type][t] + lv->x.refaulted[type][t]) / 2;
		lv->x.avg_total[type][t] =
			(lv->x.avg_total[type][t] + lv->x.protected[type][t] +
			 lv->x.evicted[type][t]) / 2;
		lv->x.refaulted[type][t] = 0;
		lv->x.evicted[type][t] = 0;
		lv->x.protected[type][t] = 0;
	}
}

/* lru_gen_folio_seq(): where a folio with these flags goes. */
static __always_inline u64 folio_seq(struct te_lruvec *lv, int type, u32 f,
				     u32 refs, bool ws)
{
	u64 gen, seq;

	if (f & TIER_EXT_F_ACTIVE)
		gen = MIN_GENS - ws;
	else if ((!type && !(f & TIER_EXT_F_SWAPCACHE)) ||
		 (f & (TIER_EXT_F_DIRTY | TIER_EXT_F_WRITEBACK)))
		gen = MIN_GENS;
	else
		gen = NR_GENS - (ws || refs);

	seq = lv->x.max_seq + 1 - gen;
	if (seq < lv->x.min_seq[type & 1])
		seq = lv->x.min_seq[type & 1];
	return seq;
}

/* ----------------------------------------------------------- the PID */

struct te_ctrl_pos {
	u64 refaulted;
	u64 total;
	u64 gain;
};

/* read_ctrl_pos(); tier == NR_TIERS means all tiers */
static __always_inline void read_pos(struct te_lruvec *lv, int type, int tier,
				     u64 gain, struct te_ctrl_pos *pos)
{
	int t;

	type &= 1;
	pos->gain = gain;
	pos->refaulted = pos->total = 0;
	for (t = 0; t < NR_TIERS; t++) {
		if (tier != NR_TIERS && t != tier)
			continue;
		pos->refaulted += lv->x.avg_refaulted[type][t] +
				  lv->x.refaulted[type][t];
		pos->total += lv->x.avg_total[type][t] +
			      lv->x.protected[type][t] + lv->x.evicted[type][t];
	}
}

/* positive_ctrl_err() */
static __always_inline bool positive_err(struct te_ctrl_pos *sp,
					 struct te_ctrl_pos *pv)
{
	return pv->refaulted < LRU_BATCH ||
	       pv->refaulted * (sp->total + LRU_BATCH) * sp->gain <=
	       (sp->refaulted + 1) * pv->total * pv->gain;
}

/* get_tier_idx(): tiers above the returned one are protected */
static __always_inline int tier_idx(struct te_lruvec *lv, int type)
{
	struct te_ctrl_pos sp, pv;
	int tier;

	read_pos(lv, type, 0, 2, &sp);
	for (tier = 1; tier < NR_TIERS; tier++) {
		read_pos(lv, type, tier, 3, &pv);
		if (!positive_err(&sp, &pv))
			break;
	}
	return tier - 1;
}

/* get_type_to_scan() */
static __always_inline int type_to_scan(struct te_lruvec *lv, int swp)
{
	struct te_ctrl_pos sp, pv;

	if (swp <= 1)
		return 1;
	if (swp >= MAX_SWP)
		return 0;
	read_pos(lv, 0, NR_TIERS, swp, &sp);
	read_pos(lv, 1, NR_TIERS, MAX_SWP - swp, &pv);
	return positive_err(&sp, &pv);
}

/* ------------------------------------------------ generations (locked) */

/* try_to_inc_min_seq(): skip empty old generations */
static __always_inline void try_inc_min_seq(struct te_lruvec *lv, int swp)
{
	u64 min_seq[2], seq;
	bool inc = false;
	int type, i;

	for (type = 0; type < 2; type++) {
		min_seq[type] = lv->x.min_seq[type];
		if (type < min_type(swp) || type > max_type(swp))
			continue;
		for (i = 0; i < NR_GENS; i++) {
			if (min_seq[type] + MIN_GENS > lv->x.max_seq)
				break;
			if (lv->lists[LIST(type, min_seq[type])].nr)
				break;
			min_seq[type]++;
			inc = true;
		}
	}
	if (!inc)
		return;

	/* see the comment on lru_gen_folio: keep anon and file in step */
	if (swp && swp <= MAX_SWP) {
		seq = lv->x.max_seq - MIN_GENS;
		if (min_seq[0] > seq && min_seq[1] < seq)
			min_seq[0] = seq;
		else if (min_seq[1] > seq && min_seq[0] < seq)
			min_seq[1] = seq;
	}
	for (type = 0; type < 2; type++) {
		if (min_seq[type] <= lv->x.min_seq[type])
			continue;
		carryover(lv, type);
		lv->x.min_seq[type] = min_seq[type];
	}
}

/*
 * inc_min_seq() for a full type: move up to a batch of the oldest
 * generation into the next one (folio_inc_gen() clears the refs). Returns
 * true once the generation is empty and min_seq was advanced. Global, so
 * the verifier checks the loop once.
 */
__noinline int inc_min_seq(s32 nid, int type, int swp)
{
	struct te_lruvec *lv = te_lv(nid);
	struct te_node __arena *n;
	int done = 0, i, tier;
	u64 old, pages;
	u32 link, refs;

	if (!lv)
		return 0;
	type &= 1;
	te_lock(lv);
	lv_init(lv);
	if (nr_gens(lv, type) < NR_GENS) {
		done = 1;
		goto unlock;
	}
	old = lv->x.min_seq[type];
	/* a type that is not evictable just moves on */
	if ((type && swp == ANON_ONLY) || (!type && !swp))
		goto advance;
	bpf_for(i, 0, LRU_BATCH) {
		link = lv->lists[LIST(type, old)].tail;
		n = te_link(link);
		if (!n)
			break;
		refs = n->meta;
		if (refs + (n->flags & F_WS) != REFS_MAX + 1) {
			tier = tier_of(refs, n->flags & F_WS) & 3;
			pages = te_node_pages(n);
			lv->x.protected[type][tier] += pages;
		}
		n->meta = 0;
		te_push_tail(lv, LIST(type, old + 1), link - 1, n);
	}
	if (lv->lists[LIST(type, old)].nr)
		goto unlock;
advance:
	carryover(lv, type);
	lv->x.min_seq[type] = old + 1;
	done = 1;
unlock:
	te_unlock(lv);
	return done;
}

/*
 * Aging: should_run_aging() and inc_max_seq(). Returns 1 if max_seq was
 * bumped (and the kernel should walk the page tables), 0 if aging is not
 * needed, -1 if the oldest generation is still being moved (inc_min_seq()
 * works in batches; the next call continues).
 */
__noinline int age(s32 nid, int swp, int priority)
{
	struct te_lruvec *lv = te_lv(nid);
	u64 emin, max_seq;
	int need, ok;

	if (!lv)
		return 0;
	te_lock(lv);
	lv_init(lv);
	try_inc_min_seq(lv, swp);
	emin = evictable_min_seq(lv, swp);
	max_seq = lv->x.max_seq;
	if (emin + MIN_GENS > max_seq)
		need = 1;	/* eviction is not possible anymore */
	else if (priority == DEF_PRIO)
		need = 0;	/* be lazy at the default priority */
	else
		need = emin + MIN_GENS == max_seq;
	te_unlock(lv);
	if (!need)
		return 0;

	/* make room for a new generation */
	ok = inc_min_seq(nid, 0, swp);
	ok &= inc_min_seq(nid, 1, swp);
	if (!ok)
		return -1;	/* more to move; the next batch continues */

	te_lock(lv);
	if (lv->x.max_seq == max_seq && nr_gens(lv, 0) < NR_GENS &&
	    nr_gens(lv, 1) < NR_GENS)
		lv->x.max_seq = max_seq + 1;
	else
		ok = 0;
	te_unlock(lv);
	if (ok)
		__sync_fetch_and_add(&nr_aged, 1);
	return ok;
}

/*
 * Evict from the oldest generation of @type: sort_folio() protects folios
 * in tiers above @tidx (moving them to the next generation), the others are
 * nominated. Returns the pages nominated.
 */
__noinline int scan(struct tier_ext_victims_ctx *ctx __arg_trusted, int type,
		    int tidx)
{
	struct te_lruvec *lv;
	struct te_node __arena *n;
	u64 got = 0, pages, want, oldest;
	u32 link, refs;
	int i, tier;
	bool ws;

	if (!ctx)
		return 0;
	lv = te_lv(ctx->nid);
	if (!lv)
		return 0;
	type &= 1;
	want = ctx->nr_requested;

	te_lock(lv);
	lv_init(lv);
	oldest = lv->x.min_seq[type];
	/* the two youngest generations are never evicted: need aging */
	if (nr_gens(lv, type) <= MIN_GENS)
		goto unlock;
	bpf_for(i, 0, 4 * TIER_EXT_MAX_VICTIMS) {
		if (got >= want)
			break;
		link = lv->lists[LIST(type, oldest)].tail;
		n = te_link(link);
		if (!n)
			break;
		refs = n->meta;
		ws = n->flags & F_WS;
		tier = tier_of(refs, ws) & 3;
		if (tier > tidx || refs + ws == REFS_MAX + 1) {
			/* protected: folio_inc_gen() */
			if (refs + ws != REFS_MAX + 1)
				lv->x.protected[type][tier] += te_node_pages(n);
			n->meta = 0;
			te_push_head(lv, LIST(type, oldest + 1), link - 1, n);
			__sync_fetch_and_add(&nr_protected, 1);
			continue;
		}
		pages = te_nominate(ctx, lv, link - 1, n);
		if (!pages)
			break;
		got += pages;
	}
	if (!lv->lists[LIST(type, oldest)].nr)
		try_inc_min_seq(lv, eff_swappiness(ctx));
unlock:
	te_unlock(lv);
	__sync_fetch_and_add(&nr_pick[type], got);
	return got;
}

/* ------------------------------------------------------------- events */

TIER_EXT_EVENT(folio_added)
{
	struct te_node __arena *n = te_node(ctx->pfn);
	struct te_lruvec *lv = te_lv(ctx->nid);
	struct ghost *g = NULL, gcopy = {};
	u64 key = ctx->data_key, seq;
	int type = te_type(ctx), tier;
	bool returned = te_returned(ctx), refault = false, ws;
	u32 refs;

	if (!n || !lv)
		return;

	/* a refault: data we evicted comes back */
	if (!returned && key) {
		g = bpf_map_lookup_elem(&ghosts, &key);
		if (g) {
			gcopy = *g;
			bpf_map_delete_elem(&ghosts, &key);
			refault = gcopy.type == type;
		}
	}

	te_lock(lv);
	lv_init(lv);
	if (ctx->folio_flags & TIER_EXT_F_UNEVICTABLE) {
		te_unlink(lv, n);
		goto unlock;
	}
	if (n->list && !returned)
		goto unlock;	/* already tracked (putback after isolation) */

	if (returned) {
		refs = n->meta;
		ws = (n->flags & F_WS) || (ctx->folio_flags & TIER_EXT_F_WORKINGSET);
	} else {
		refs = ctx->engine == TIER_EXT_ENGINE_MGLRU ? ctx->lru_refs :
		       !!(ctx->folio_flags & TIER_EXT_F_REFERENCED);
		ws = ctx->folio_flags & TIER_EXT_F_WORKINGSET;
		/* lru_gen_refault(): only recent evictions count */
		if (refault && lv->x.max_seq - gcopy.token < NR_GENS &&
		    gcopy.token <= lv->x.max_seq) {
			tier = tier_of(gcopy.refs, gcopy.ws) & 3;
			lv->x.refaulted[type & 1][tier] += ctx->nr_pages;
			if (gcopy.ws)
				ws = true;
			else
				refs = gcopy.refs;
		} else {
			refault = false;
		}
	}
	if (refs > REFS_MAX)
		refs = REFS_MAX;

	n->order = te_order(ctx->nr_pages);
	n->meta = refs;
	n->flags = (ws ? F_WS : 0) | F_OWN;
	seq = folio_seq(lv, type, ctx->folio_flags, refs, ws);
	/*
	 * evict_folios(): "don't add rejected folios to the oldest
	 * generation" - a victim the kernel could not reclaim now (locked
	 * for read-ahead I/O, under writeback) would otherwise be nominated
	 * again right away. The kernel marks it active; so do we.
	 */
	if (returned && seq == lv->x.min_seq[type & 1])
		seq = folio_seq(lv, type, ctx->folio_flags | TIER_EXT_F_ACTIVE,
				refs, ws);
	te_push_head(lv, LIST(type, seq), ctx->pfn, n);
unlock:
	te_unlock(lv);

	if (returned)
		__sync_fetch_and_add(&nr_returned, 1);
	else
		__sync_fetch_and_add(&nr_added, 1);
	if (refault)
		__sync_fetch_and_add(&nr_refaults, 1);
}

/* folio_mark_accessed(): lru_gen_inc_refs() */
TIER_EXT_EVENT(folio_accessed)
{
	struct te_node __arena *n = te_node(ctx->pfn);
	struct te_lruvec *lv = te_lv(ctx->nid);

	if (!n || !lv)
		return;
	te_lock(lv);
	if (n->list) {
		if (n->meta < REFS_MAX)
			n->meta++;
		else
			n->flags |= F_WS;
	}
	te_unlock(lv);
}

/* young PTEs found by a page table walk: folio_update_gen() */
TIER_EXT_EVENT(folio_referenced)
{
	struct te_node __arena *n = te_node(ctx->pfn);
	struct te_lruvec *lv = te_lv(ctx->nid);
	int type = te_type(ctx), promoted = 0;

	if (!n || !lv)
		return;
	te_lock(lv);
	lv_init(lv);
	if (!n->list)
		goto unlock;
	if (!n->meta && !(n->flags & F_WS)) {
		n->meta = 1;	/* first sighting: PG_referenced */
		goto unlock;
	}
	n->meta = 0;
	n->flags |= F_WS;
	te_push_head(lv, LIST(type, lv->x.max_seq), ctx->pfn, n);
	promoted = 1;
unlock:
	te_unlock(lv);
	if (promoted)
		__sync_fetch_and_add(&nr_promoted, 1);
}

TIER_EXT_EVENT(folio_evicted)
{
	struct te_node __arena *n = te_node(ctx->pfn);
	struct te_lruvec *lv = te_lv(ctx->nid);
	struct ghost g = {};
	u64 key = ctx->data_key;
	int type = te_type(ctx), tier;
	bool known;

	if (!n || !lv)
		return;
	te_lock(lv);
	lv_init(lv);
	/* nominated folios are unlinked but keep their refs and flags */
	known = n->flags & F_OWN;
	if (known) {
		g.refs = n->meta;
		g.ws = n->flags & F_WS;
	}
	g.type = type;
	g.token = lv->x.min_seq[type & 1];
	tier = tier_of(g.refs, g.ws) & 3;
	lv->x.evicted[type & 1][tier] += ctx->nr_pages;
	te_unlink(lv, n);
	n->flags = 0;
	te_unlock(lv);

	if (key)
		bpf_map_update_elem(&ghosts, &key, &g, BPF_ANY);
}

TIER_EXT_EVENT(folio_removed)
{
	struct te_node __arena *n = te_node(ctx->pfn);
	struct te_lruvec *lv = te_lv(ctx->nid);

	if (!n || !lv)
		return;
	te_lock(lv);
	te_unlink(lv, n);
	n->flags = 0;
	te_unlock(lv);
}

TIER_EXT_EVENT(folio_migrated)
{
	te_migrate(ctx);
}

/* ------------------------------------------------------ pick_victims */

TIER_EXT_OP(pick_victims, tier_ext_victims_ctx)
{
	struct te_lruvec *lv = te_lv(ctx->nid);
	int swp = eff_swappiness(ctx), type, tidx, ret = 0, aged;
	u64 got;

	if (!lv)
		return 0;

	/* aging first, like try_to_shrink_lruvec() */
	aged = age(ctx->nid, swp, ctx->priority);
	if (aged > 0) {
		__sync_fetch_and_add(&nr_age_requests, 1);
		ret = TIER_EXT_AGE;
	} else if (aged < 0) {
		ret = TIER_EXT_AGAIN;
	}

	/* PID decisions read the stats unlocked: they are heuristics */
	type = type_to_scan(lv, swp);
	if (!te_may(ctx, type))
		type ^= 1;
	tidx = tier_idx(lv, type);
	got = scan(ctx, type, tidx);
	/* this type needs aging first: try the other one */
	if (!got && te_may(ctx, type ^ 1))
		got = scan(ctx, type ^ 1, tier_idx(lv, type ^ 1));
	/* protected folios moved up a generation: keep going */
	if (got < ctx->nr_requested && lv->lists[LIST(type, lv->x.min_seq[type & 1])].nr)
		ret |= TIER_EXT_AGAIN;
	return ret;
}

SEC(".struct_ops.link")
struct tier_ext_ops mglru = {
	.folio_added	  = (void *)folio_added,
	.folio_accessed	  = (void *)folio_accessed,
	.folio_referenced = (void *)folio_referenced,
	.folio_evicted	  = (void *)folio_evicted,
	.folio_removed	  = (void *)folio_removed,
	.folio_migrated	  = (void *)folio_migrated,
	.pick_victims	  = (void *)pick_victims,
	.name		  = "mglru",
};
