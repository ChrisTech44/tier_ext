# Architecture

## Big picture

```
 user space                              kernel
 ──────────                              ──────
 policy.bpf.o ──load──▶ BPF verifier ──▶ struct tier_ext_ops (BPF programs)
 tier_ext_load                                   │ reg(): attach
   -c /sys/fs/cgroup/db  ──▶ cgroup_id           ▼
   -n 0                  ──▶ node_mask   struct tier_ext_policy ◀── lruvec(db, node 0)->tier_ext
                                                 ▲
                           reclaim (vmscan.c) ───┘ "is there a policy on this lruvec?"
                                                     no  → normal LRU / MGLRU
                                                     yes → ask it, sanitize the answer
```

There are three pieces:

1. **The interface** (`include/linux/tier_ext.h`): `struct tier_ext_ops`, the three
   context structs the callbacks receive, the verdict values they return, and
   one kfunc (`bpf_tier_ext_add_victim()`).
2. **The glue** (`mm/tier_ext.c`): registers `tier_ext_ops` as a BPF struct_ops
   type, attaches and detaches policies, and implements the `__tier_ext_*()`
   dispatch functions that build a context, call the BPF program, and check its
   answer.
3. **The hook sites** (`mm/vmscan.c`, `mm/folio.c`, `mm/migrate.c`,
   `mm/huge_memory.c`): a handful of short calls placed where reclaim makes
   each decision and where folios enter, leave or move.

## The interface

```c
struct tier_ext_ops {
	/* decisions */
	s32  (*lruvec_shrink)(struct tier_ext_lruvec_ctx *ctx);  /* SKIP or DEFAULT        */
	s32  (*swappiness)(struct tier_ext_lruvec_ctx *ctx);     /* 0..200, <0 = default    */
	s32  (*folio_scan)(struct tier_ext_folio_ctx *ctx);      /* RECLAIM / KEEP / DEFAULT */
	s32  (*folio_reclaim)(struct tier_ext_folio_ctx *ctx);   /* same, | NO_DEMOTE / DEMOTE_ONLY */
	/* events */
	void (*folio_added)(struct tier_ext_folio_ctx *ctx);     /* also RETURNED, SPLIT */
	void (*folio_accessed)(struct tier_ext_folio_ctx *ctx);
	void (*folio_evicted)(struct tier_ext_folio_ctx *ctx);
	/* victim nomination */
	s32  (*pick_victims)(struct tier_ext_victims_ctx *ctx);  /* 0 | AGE | AGAIN */
	/* more events */
	void (*folio_removed)(struct tier_ext_folio_ctx *ctx);
	void (*folio_migrated)(struct tier_ext_folio_ctx *ctx);
	void (*folio_referenced)(struct tier_ext_folio_ctx *ctx);
	/* target */
	u64  cgroup_id;   /* 0 = root cgroup   */
	u64  node_mask;   /* 0 = all nodes     */
	u64  flags;       /* reserved, 0       */
	char name[32];
};
```

Every callback is optional; a missing one means "always default".

The context structs carry a snapshot of what the policy needs, filled by the
kernel: for an lruvec its cgroup id, node, engine (classic or MGLRU), reclaim
priority, the default swappiness, reclaim progress and the anon/file sizes; for
a folio its pfn (a stable id while it is in memory), node, size, a flags word
(anon, active, referenced, dirty, mapped, ...), MGLRU generation and access
count, the number of page-table references found (for `folio_reclaim`), and the
reclaim priority, and a `data_key` that identifies the data independent of the
page frame (file offset or swap slot, D23). The raw `struct folio *` is
included too, and BPF can read (but never write) any of its fields through
BTF. `pick_victims` gets a third context, described below.

## Attaching: from (cgroup, nodes) to lruvecs

When user space attaches the struct_ops map, the kernel calls
`tier_ext_ops_reg()` → `tier_ext_attach()`:

1. Resolve `node_mask` to a set of possible nodes (0 = all). A bit for a node
   that cannot exist is an error (`EINVAL`).
2. Resolve `cgroup_id` to a memcg and take a reference so it cannot be freed
   while the policy is attached. The id must name a cgroup that has the memory
   controller enabled itself; the kernel does not silently fall back to an
   ancestor (`EINVAL`, or `ENOENT` for an unknown id).
3. Under `tier_ext_mutex`, check that none of the target lruvecs already has a
   policy (`EBUSY`). One lruvec, at most one policy.
4. Allocate a `struct tier_ext_policy` (per-CPU counters, target, ops pointer),
   enable the global static key, and publish the policy in each target lruvec
   with `rcu_assign_pointer(lruvec->tier_ext, policy)`.

The only change to core data structures is that one pointer in `struct lruvec`.

Detaching (`tier_ext_ops_unreg()`, when the link is destroyed or the loader
exits) clears the pointers, waits for an RCU grace period so no callback can
still be running, decrements the static key, and drops the memcg reference.
From that moment the lruvecs behave exactly as before.

Replacing (`tier_ext_ops_update()`, `BPF_LINK_UPDATE`) swaps the programs of
an attached policy in place, for the same target, without a moment in which
the lruvecs have no policy (D30).

## Where the kernel asks: the hook points

Reclaim takes the same shape in both engines. For each lruvec it decides how
hard to scan, scans the cold end of its lists and isolates candidate folios,
then tries to free each candidate. tier_ext hooks each of these steps.

```
classic LRU                                   MGLRU
───────────                                   ─────
shrink_node_memcgs()                          shrink_many() / lru_gen_shrink_lruvec()
 └ shrink_lruvec(lruvec)                       └ try_to_shrink_lruvec(lruvec)
    ├─[1] lruvec_shrink  → SKIP? return          ├─[1] lruvec_shrink  → SKIP? return
    ├ get_scan_count()                           ├ get_swappiness()
    │  └─[2] swappiness                          │  └─[2] swappiness
    └ shrink_inactive/active_list()              └ evict_folios()
       └ isolate_lru_folios()                       └ scan_folios()
          └─[3] folio_scan (each folio,               └ sort_folio()
               LRU lock held)                            └─[3] folio_scan (each folio)
       └ shrink_folio_list()  ◀── shared by both engines ──┘
          ├ folio_check_references()
          │  └─[4] folio_reclaim (folio isolated + locked, refs counted)
          ├ demote to slower node?  ◀── NO_DEMOTE / DEMOTE_ONLY
          ├ swap out / write back / drop
          └ free ──[E3] folio_evicted

lru_add() (a folio enters an LRU list) ──[E1] folio_added
folio_mark_accessed() (read()/write() touched a page cache folio) ──[E2] folio_accessed
```

With victim nomination (below) there is one more step before the engine scans:

```
shrink_lruvec() / try_to_shrink_lruvec(), after [1] and [2]:
 └─[V] tier_ext_reclaim_victims()        (mm/vmscan.c)
        repeat: pick_victims ─▶ validate + isolate pfns ─▶ shrink_folio_list()
                               ─▶ hand survivors back (folio_added, RETURNED)
        then the engine scans with what is left of its budget
```

### [1] `lruvec_shrink`: skip this lruvec?

Called once per lruvec per reclaim pass, before any scanning. Returning
`TIER_EXT_SKIP` makes reclaim move on to the next lruvec. Example: `protect`
keeps a cgroup's DRAM-node footprint above a floor, like a per-node
`memory.low`. **Ignored at priority 0** (see [safety.md](safety.md)).

### [2] `swappiness`: anon/file balance for this lruvec

The kernel computes its swappiness (from `vm.swappiness`, or the value passed to
`memory.reclaim`) and passes it in `ctx->swappiness`. The policy may return a
replacement in 0..200. The kernel's own guards still apply afterwards: no swap
space means no anon reclaim, whatever the policy says, and a `memory.reclaim`
request for anon only is never overridden.

### [3] `folio_scan`: take this folio off the list?

Called for each folio the scanner looks at, with the LRU lock held.

| Verdict | classic engine | MGLRU |
|---|---|---|
| `DEFAULT` | normal isolation | normal: tier/refs protection may promote it |
| `RECLAIM` | isolate it (same as default today) | isolate it, **bypassing tier protection** |
| `KEEP` | rotate it to the head of its list | move it to the **youngest generation** |

Hard constraints are never overridden: unevictable folios, folios in a zone
this reclaim may not use, and folios being freed or isolated by someone else.

### [4] `folio_reclaim`: reclaim this folio now?

The folio is isolated and locked, and the kernel has just counted its
page-table references (`ctx->referenced_ptes`). This is the point where the
kernel would say "referenced, give it another chance" or "reclaim it".

| Verdict | effect |
|---|---|
| `DEFAULT` | the kernel's reference-based decision |
| `RECLAIM` | reclaim it even though it was referenced |
| `KEEP` | put it back as active (MGLRU: youngest generation) |

`RECLAIM` and `DEFAULT` may be OR-ed with a tiering flag:

* `TIER_EXT_NO_DEMOTE`: do not demote this folio; free it (swap out or drop).
* `TIER_EXT_DEMOTE_ONLY`: demote it or keep it. If demotion is disabled,
  impossible (last tier) or fails, the folio stays in memory.

Folios in `VM_LOCKED` VMAs (mlock) and folios whose rmap lock was contended
never reach the policy; the kernel's handling of those is not negotiable.

### [V] `pick_victims`: nominate victims

This is how a policy chooses victims itself instead of judging the kernel's
candidates (the design is D19–D22 in [decisions.md](decisions.md)). Before
the engine scans an lruvec, `tier_ext_reclaim_victims()` calls
`pick_victims` in batches until the engine's planned scan budget for this pass
is used, the reclaim target is met, or the policy returns nothing valid. Each
call gets a `struct tier_ext_victims_ctx`:

| Field | Meaning |
|---|---|
| `nr_requested` | pages the kernel would like from this batch (what is left of the budget) |
| `batch` | 0 for the first call of this reclaim pass |
| `flags` | `TIER_EXT_VF_MAY_ANON` (swap available and allowed, swappiness > 0), `_MAY_FILE`, `_MAY_UNMAP`, `_CAN_AGE` |
| `swappiness`, `priority`, `engine`, `reclaim_flags` | as in the other contexts |
| `nr_anon`, `nr_file` | the lruvec's LRU sizes |
| `nr_scan_anon`, `nr_scan_file` | the engine's own anon/file split of this pass (classic: `get_scan_count()`; MGLRU: the type it would scan) |
| `nr_to_reclaim`, `nr_reclaimed` | the whole pass's target and progress |

The policy adds up to 32 pfns with `bpf_tier_ext_add_victim(ctx, pfn)` and
returns 0, or flags:

* `TIER_EXT_AGE`: run the engine's aging after this batch (MGLRU page table
  walk; at most once per pass).
* `TIER_EXT_AGAIN`: "nothing yet, but I made progress, call me again"
  (at most 16 empty batches in a row).

The kernel then, for each pfn: checks that it is online memory, takes a
reference on a head page, checks that the folio is on this lruvec's LRU
(anything else is counted as `victims_invalid` or `victims_foreign` and
left alone), applies the pass's restrictions (zone, anon/file, mapped: a
folio that fails them is `victims_skipped` and handed back), isolates it,
and reclaims the batch with `shrink_folio_list()`: the same reference check,
writeback, swap-out and demotion logic as any other reclaim, with
`folio_reclaim` still consulted (its context carries `TIER_EXT_RC_NOMINATED`).
Folios that survive are handed back to the policy through `folio_added` with
`TIER_EXT_F_RETURNED` and go back on the kernel's LRU.

What the victims consumed is taken out of the engine's scan budget, and the
engine scans the rest as before, so a policy that nominates nothing changes
nothing. At priority 0 the engine gets its full budget regardless.

### Events [E1..E6]

* `folio_added`: a folio was put on this lruvec's LRU. This covers new
  folios and folios put back after a failed reclaim attempt. Two flags mark
  special cases: `TIER_EXT_F_RETURNED` (a victim this policy nominated, handed
  back; see above) and `TIER_EXT_F_SPLIT` (a tail split off a large folio).
* `folio_accessed`: `folio_mark_accessed()` ran, i.e. a system-call level
  access to page cache. (Accesses through page tables are not events; they
  show up as `referenced_ptes` and MGLRU generations.)
* `folio_evicted`: reclaim freed the folio. Demoted folios are not "evicted";
  they appear as `folio_added` on the destination lruvec. `data_key` is the
  identity the data had before it left the page or swap cache.
* `folio_removed`: the folio was freed without reclaim (truncation, unmap,
  exit), from `__page_cache_release()`, or from `move_folios_to_lru()` when
  the last reference went away while it was isolated.
* `folio_migrated`: the folio moved to another page frame (compaction, NUMA
  balancing, demotion); `old_pfn`/`old_nid` say where it was. Delivered to
  the policy of the *old* lruvec, before the new folio is added to its LRU.
* `folio_referenced`: MGLRU found the folio's page table entries young, in a
  page table walk (aging) or in look-around (eviction). This is the only
  access signal for anon memory and mmap'd files between reclaim attempts
  (D26). It only exists on MGLRU.

Events let a policy keep its own per-folio metadata in BPF maps, keyed by
`ctx->pfn`. For example, `lfu` counts accesses per folio and protects the hot
ones. Engine policies keep complete orderings of the folios this way; see
[engines.md](engines.md).

## Dispatch: what happens on each call

Each hook site checks, inline, `static_branch_unlikely(&tier_ext_attached) &&
lruvec->tier_ext != NULL`. With no policy attached anywhere the static key is
off and the check is a patched-out jump. If a policy is attached, the
`__tier_ext_*()` function:

1. enters an RCU read-side section and loads the policy pointer,
2. fills the context struct on the stack,
3. calls the BPF program,
4. sanitizes the return value: unknown values count as `invalid` and become
   `DEFAULT`, `KEEP`/`SKIP`/`DEMOTE_ONLY` become `DEFAULT` at priority 0,
5. bumps per-CPU counters, leaves the RCU section, returns the verdict.

## Observability

`/sys/kernel/debug/tier_ext` lists every attached policy with its target and
counters:

```
policy observe cgroup 4465 nodes 0-1
  shrink_calls 96
  scan_calls 981
  reclaim_calls 981
  folio_added 4096
  folio_evicted 981
  overridden 0
  invalid 0
  ...
```

`overridden` counts KEEP/SKIP/DEMOTE_ONLY answers the kernel ignored because
reclaim reached priority 0. `invalid` counts unsupported return values. Both
are the first things to check when a policy misbehaves.

For victim nomination: `victims_calls`, `victims_nominated` (pfns handed
over), `victims_isolated` (taken for reclaim), `victims_invalid` (not an LRU
folio), `victims_foreign` (another lruvec's folio), `victims_skipped` (not
reclaimable in this pass, handed back), `victims_busy` (isolated by someone
else meanwhile), `victims_returned` (handed back after reclaim), `victims_age`
and `victims_again` (the two return flags). A healthy engine has
`isolated == nominated` and `invalid == 0`.
