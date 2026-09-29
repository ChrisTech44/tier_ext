// SPDX-License-Identifier: GPL-2.0
/*
 * lhd: Least Hit Density (Beckmann et al., NSDI '18), page cache. Port of
 * cache_ext's cache_ext_lhd.bpf.c.
 *
 * Each folio belongs to a class (by its last two hit ages) and has an age
 * (requests since its last access, coarsened). Per class and age the policy
 * learns how many hits and evictions happened; from those it models the
 * expected hits per unit of cache space and time ("hit density"). Victims
 * are the lowest-density folios of a sample of 16 (sampling like cache_ext).
 *
 * The model is rebuilt every REQS_PER_RECONFIG requests by te_periodic, a
 * syscall program the loader runs once a second (cache_ext's loader ran its
 * "reconfigure" program on a ring buffer event instead).
 *
 * Differences from cache_ext: only the classes it actually used (its app id
 * was a constant) are kept, and the tables live in the BPF arena. Anon
 * memory is left to the kernel's engine.
 */
#include "tier_ext.bpf.h"

#define TE_NR_LISTS 1
struct te_extra {};
static int lhd_init(void);
#define TE_POLICY_INIT lhd_init
#include "te_engine.bpf.h"

char _license[] SEC("license") = "GPL";

#define NR_CLASSES			16	/* HIT_AGE_CLASSES */
#define MAX_AGE				(1 << 14)
#define MAX_AGE_MASK			(MAX_AGE - 1)
#define INITIAL_AGE_COARSENING_SHIFT	10
#define REQS_PER_RECONFIG		(1 << 20)
#define HIT_SCALING_FACTOR		(1 << 20)
#define HIT_DENSITY_SCALING_FACTOR	(1 << 20)
#define NUM_OBJECTS_SCALING_FACTOR	(1 << 20)
#define TOTAL_EVENTS_THRESH		(HIT_SCALING_FACTOR / 100000)
#define AGE_COARSENING_ERROR_TOLERANCE	100
#define SAMPLE				16

struct lhd_class {
	u64 total_hits;
	u64 total_evictions;
	u64 hits[MAX_AGE];
	u64 evictions[MAX_AGE];
	u64 hit_densities[MAX_AGE];
};

/* per pfn, next to te_nodes */
struct lhd_meta {
	u64 last_access;
	u32 last_hit_age;
	u32 last_last_hit_age;
};

struct lhd_class __arena *__arena_global classes;
struct lhd_meta __arena *__arena_global metas;

u64 timestamp, requests, num_objects, num_reconfigurations, overflows;
u64 age_coarsening_shift = INITIAL_AGE_COARSENING_SHIFT;
u64 ewma_num_objects, ewma_num_objects_mass;
u64 nr_picked;

static int lhd_init(void)
{
	struct lhd_class __arena *c;
	u64 pages;
	int i, j;

	pages = (te_max_pfn * sizeof(struct lhd_meta) + 4095) / 4096;
	metas = bpf_arena_alloc_pages(&te_arena, NULL, pages, NUMA_NO_NODE, 0);
	pages = (NR_CLASSES * sizeof(struct lhd_class) + 4095) / 4096;
	c = bpf_arena_alloc_pages(&te_arena, NULL, pages, NUMA_NO_NODE, 0);
	if (!metas || !c)
		return -12;
	classes = c;
	/* initial hit densities: GDSF-like, (class + 1) / (age + 1) */
	bpf_for(i, 0, NR_CLASSES) {
		bpf_for(j, 0, MAX_AGE)
			c[i].hit_densities[j] = (u64)HIT_DENSITY_SCALING_FACTOR *
						(i + 1) / (j + 1);
	}
	return 0;
}

static __always_inline struct lhd_meta __arena *meta_of(u64 pfn)
{
	struct lhd_meta __arena *m = metas;

	if (!m || pfn >= te_max_pfn)
		return NULL;
	return &m[pfn];
}

static __always_inline u32 hit_age_class(u64 hit_age)
{
	u32 class = 0;
	int i;

	if (!hit_age)
		return 0;
	/* approximates log(MAX_AGE - hit_age) */
	for (i = 0; i < NR_CLASSES - 1; i++) {
		if (hit_age >= MAX_AGE)
			break;
		hit_age <<= 1;
		class++;
	}
	return class;
}

static __always_inline struct lhd_class __arena *class_of(struct lhd_meta __arena *m)
{
	struct lhd_class __arena *c = classes;

	if (!c)
		return NULL;
	return &c[hit_age_class((u64)m->last_hit_age + m->last_last_hit_age) &
		  (NR_CLASSES - 1)];
}

static __always_inline u64 age_of(struct lhd_meta __arena *m)
{
	u64 age = (timestamp - m->last_access) >> (age_coarsening_shift & 63);

	if (age >= MAX_AGE) {
		__sync_fetch_and_add(&overflows, 1);
		return MAX_AGE - 1;
	}
	return age;
}

static __always_inline u64 hit_density(struct lhd_meta __arena *m)
{
	struct lhd_class __arena *c = class_of(m);
	u64 age = age_of(m);

	if (!c || age == MAX_AGE - 1)
		return 0;
	return c->hit_densities[age & MAX_AGE_MASK];
}

TIER_EXT_EVENT(folio_added)
{
	struct te_node __arena *n = te_node(ctx->pfn);
	struct lhd_meta __arena *m = meta_of(ctx->pfn);
	struct te_lruvec *lv = te_lv(ctx->nid);
	bool fresh = false;

	if (te_anon(ctx) || !n || !m || !lv)
		return;
	te_lock(lv);
	if (te_returned(ctx)) {
		te_push_head(lv, 0, ctx->pfn, n);
	} else if (!n->list) {
		n->order = te_order(ctx->nr_pages);
		m->last_access = timestamp;
		m->last_hit_age = 0;
		m->last_last_hit_age = MAX_AGE;
		te_push_head(lv, 0, ctx->pfn, n);
		fresh = true;
	}
	te_unlock(lv);
	if (fresh) {
		__sync_fetch_and_add(&timestamp, 1);
		__sync_fetch_and_add(&requests, 1);
		__sync_fetch_and_add(&num_objects, 1);
	}
}

TIER_EXT_EVENT(folio_accessed)
{
	struct te_node __arena *n = te_node(ctx->pfn);
	struct lhd_meta __arena *m = meta_of(ctx->pfn);
	struct lhd_class __arena *c;
	u64 age;

	if (te_anon(ctx) || !n || !m || !n->list)
		return;
	age = age_of(m);
	c = class_of(m);
	if (!c)
		return;
	m->last_last_hit_age = m->last_hit_age;
	m->last_hit_age = age;
	m->last_access = timestamp;
	__sync_fetch_and_add(&c->hits[age & MAX_AGE_MASK], HIT_SCALING_FACTOR);
	__sync_fetch_and_add(&timestamp, 1);
	__sync_fetch_and_add(&requests, 1);
}

TIER_EXT_EVENT(folio_evicted)
{
	struct lhd_meta __arena *m = meta_of(ctx->pfn);
	struct lhd_class __arena *c;
	u64 age;

	te_forget(ctx->nid, ctx->pfn);
	if (te_anon(ctx) || !m)
		return;
	age = age_of(m);
	c = class_of(m);
	if (!c)
		return;
	__sync_fetch_and_add(&c->evictions[age & MAX_AGE_MASK], HIT_SCALING_FACTOR);
	__sync_fetch_and_sub(&num_objects, 1);
}

TIER_EXT_EVENT(folio_removed)
{
	te_forget(ctx->nid, ctx->pfn);
	if (!te_anon(ctx))
		__sync_fetch_and_sub(&num_objects, 1);
}

TIER_EXT_EVENT(folio_migrated)
{
	struct lhd_meta __arena *o = meta_of(ctx->old_pfn);
	struct lhd_meta __arena *m = meta_of(ctx->pfn);

	if (o && m) {
		m->last_access = o->last_access;
		m->last_hit_age = o->last_hit_age;
		m->last_last_hit_age = o->last_last_hit_age;
	}
	te_migrate(ctx);
}

/* sample SAMPLE folios from the old end, nominate the lowest hit density */
__noinline int pick(struct tier_ext_victims_ctx *ctx __arg_trusted)
{
	struct te_lruvec *lv;
	struct te_node __arena *n, *bn;
	struct lhd_meta __arena *m;
	u64 got = 0, pages, score, best_score = ~0ULL;
	u32 link, best = 0;
	int i;

	if (!ctx)
		return 0;
	lv = te_lv(ctx->nid);
	if (!lv)
		return 0;
	te_lock(lv);
	bpf_for(i, 0, SAMPLE * TIER_EXT_MAX_VICTIMS) {
		if (got >= ctx->nr_requested)
			break;
		link = lv->lists[0].tail;
		n = te_link(link);
		if (!n)
			break;
		m = meta_of(link - 1);
		score = m ? hit_density(m) : 0;
		te_unlink(lv, n);
		if (!best || score < best_score) {
			bn = te_link(best);
			if (bn)
				te_push_head(lv, 0, best - 1, bn);
			best = link;
			best_score = score;
		} else {
			te_push_head(lv, 0, link - 1, n);
		}
		if (i % SAMPLE == SAMPLE - 1) {
			bn = te_link(best);
			if (bn) {
				pages = te_nominate(ctx, lv, best - 1, bn);
				if (!pages) {
					te_push_head(lv, 0, best - 1, bn);
					best = 0;
					break;
				}
				got += pages;
			}
			best = 0;
		}
	}
	bn = te_link(best);
	if (bn) {
		pages = te_nominate(ctx, lv, best - 1, bn);
		if (pages)
			got += pages;
		else
			te_push_head(lv, 0, best - 1, bn);
	}
	te_unlock(lv);
	__sync_fetch_and_add(&nr_picked, got);
	return 0;
}

TIER_EXT_OP(pick_victims, tier_ext_victims_ctx)
{
	if (!te_may(ctx, 1))
		return 0;
	return pick(ctx);
}

TIER_EXT_OP(folio_reclaim, tier_ext_folio_ctx)
{
	if ((ctx->reclaim_flags & TIER_EXT_RC_NOMINATED) && !te_anon(ctx))
		return TIER_EXT_RECLAIM;
	return TIER_EXT_DEFAULT;
}

/* ------------------------------------------ model rebuild (te_periodic) */

static __always_inline u64 decay(u64 v)
{
	return v * 9 / 10;
}

static void update_classes(struct lhd_class __arena *c)
{
	int i, j;

	bpf_for(i, 0, NR_CLASSES) {
		u64 th = 0, te = 0;

		bpf_for(j, 0, MAX_AGE) {
			c[i].hits[j] = decay(c[i].hits[j]);
			c[i].evictions[j] = decay(c[i].evictions[j]);
			th += c[i].hits[j];
			te += c[i].evictions[j];
		}
		c[i].total_hits = th;
		c[i].total_evictions = te;
	}
}

/* coarser ages: fold 2^delta ages into one */
static void compress(struct lhd_class __arena *c, int delta)
{
	int i, j, k;

	bpf_for(i, 0, NR_CLASSES) {
		bpf_for(j, 0, MAX_AGE >> delta) {
			u64 h = 0, e = 0;

			bpf_for(k, 0, 1 << delta) {
				h += c[i].hits[((j << delta) + k) & MAX_AGE_MASK];
				e += c[i].evictions[((j << delta) + k) & MAX_AGE_MASK];
			}
			c[i].hits[j & MAX_AGE_MASK] = h;
			c[i].evictions[j & MAX_AGE_MASK] = e;
		}
		bpf_for(j, MAX_AGE >> delta, MAX_AGE) {
			c[i].hits[j & MAX_AGE_MASK] = 0;
			c[i].evictions[j & MAX_AGE_MASK] = 0;
		}
	}
}

/* finer ages: spread each age over 2^delta */
static void stretch(struct lhd_class __arena *c, int delta)
{
	int i, j;

	bpf_for(i, 0, NR_CLASSES) {
		bpf_for(j, 1, MAX_AGE + 1) {
			u32 idx = MAX_AGE - j;

			c[i].hits[idx & MAX_AGE_MASK] =
				c[i].hits[(idx >> delta) & MAX_AGE_MASK] >> delta;
			c[i].evictions[idx & MAX_AGE_MASK] =
				c[i].evictions[(idx >> delta) & MAX_AGE_MASK] >> delta;
		}
	}
}

static void adapt_age_coarsening(struct lhd_class __arena *c)
{
	u64 per, optimal;
	s64 delta;
	int log2 = 1, i;

	ewma_num_objects = decay(ewma_num_objects) +
			   num_objects * NUM_OBJECTS_SCALING_FACTOR;
	ewma_num_objects_mass = decay(ewma_num_objects_mass) + 1;
	if (num_reconfigurations != 5 && num_reconfigurations != 25)
		return;

	per = ewma_num_objects / (ewma_num_objects_mass ?: 1);
	optimal = per * AGE_COARSENING_ERROR_TOLERANCE / MAX_AGE;
	bpf_for(i, 1, 40) {
		if ((1ULL << log2) * NUM_OBJECTS_SCALING_FACTOR >= optimal)
			break;
		log2++;
	}
	delta = (s64)log2 - (s64)age_coarsening_shift;
	age_coarsening_shift = log2;
	ewma_num_objects *= 8;
	ewma_num_objects_mass *= 8;
	if (delta > 0 && delta < 14)
		compress(c, delta);
	else if (delta < 0 && delta > -14)
		stretch(c, -delta);
}

static void model_hit_density(struct lhd_class __arena *c)
{
	int i, j;

	bpf_for(i, 0, NR_CLASSES) {
		u64 hits = c[i].hits[MAX_AGE - 1];
		u64 events = hits + c[i].evictions[MAX_AGE - 1];
		u64 lifetime = events;

		bpf_for(j, 2, MAX_AGE + 1) {
			u32 idx = MAX_AGE - j;

			hits += c[i].hits[idx & MAX_AGE_MASK];
			events += c[i].evictions[idx & MAX_AGE_MASK];
			lifetime += events;
			c[i].hit_densities[idx & MAX_AGE_MASK] =
				events > TOTAL_EVENTS_THRESH ?
				hits * HIT_DENSITY_SCALING_FACTOR / (lifetime ?: 1) : 0;
		}
	}
}

/* Run by the loader about once a second. */
SEC("syscall")
int te_periodic(void *args)
{
	struct lhd_class __arena *c = classes;

	if (!c || requests < REQS_PER_RECONFIG)
		return 0;
	requests = 0;
	num_reconfigurations++;
	update_classes(c);
	adapt_age_coarsening(c);
	model_hit_density(c);
	overflows = 0;
	return 1;
}

SEC(".struct_ops.link")
struct tier_ext_ops lhd = {
	.folio_added	= (void *)folio_added,
	.folio_accessed	= (void *)folio_accessed,
	.folio_evicted	= (void *)folio_evicted,
	.folio_removed	= (void *)folio_removed,
	.folio_migrated	= (void *)folio_migrated,
	.pick_victims	= (void *)pick_victims,
	.folio_reclaim	= (void *)folio_reclaim,
	.name		= "lhd",
};
