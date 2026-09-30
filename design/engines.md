# Engine policies: LRU, MGLRU and cache_ext's policies in BPF

The first tier_ext policies only *judged* the folios the kernel's scanner
presented to them. With victim nomination (`pick_victims`, decisions D19–D22)
a policy can do the whole job of a reclaim algorithm: keep its own ordering
of the folios and choose every victim itself. This document calls such a
policy an **engine**, describes the library that makes engines practical to
write, and goes through every engine in `policies/`.

## How an engine works

```
            kernel                                     engine policy (BPF)
            ──────                                     ───────────────────
 lru_add() ─────────── folio_added(pfn, flags) ──────▶ put pfn on one of its lists
 folio_mark_accessed() ─ folio_accessed(pfn) ────────▶ count / move it
 MGLRU page table walk ─ folio_referenced(pfn) ──────▶ count / move it
 free, truncate ─────── folio_removed(pfn) ──────────▶ forget it
 migration ──────────── folio_migrated(old, new) ────▶ move the node to the new pfn

 reclaim of this lruvec
   └ pick_victims(ctx) ──────────────────────────────▶ take pfns from its lists,
         ◀── bpf_tier_ext_add_victim(pfn) × ≤32 ──────  hand them over
     validate + isolate each pfn
     shrink_folio_list()  (reference check, swap/writeback, demotion)
       ├ freed ──────── folio_evicted(pfn, data_key) ─▶ remember it (ghost), stats
       └ kept ───────── folio_added(pfn, RETURNED) ───▶ put it back where it belongs
     repeat until the budget is used, or the policy has nothing left
   └ the kernel's own engine reclaims whatever is left
```

A nomination is a **hand-off** (D21): the engine forgets a folio when it
nominates it, and gets it back through `folio_added` with
`TIER_EXT_F_RETURNED` if the kernel did not free it. The flags in that
context carry the kernel's verdict; `TIER_EXT_F_ACTIVE` means "the page table
entries were young, I activated it", which is how an engine learns about
accesses through mappings (the classic LRU's second chance).

Engines are an **overlay** (D28): the kernel keeps its own LRU lists or
generations as usual, so every other kernel path works unchanged and
detaching is instant.

## The library: `policies/te_engine.bpf.h`

| Piece | What it is |
|---|---|
| `te_nodes[pfn]` | one 16-byte node per page frame (prev, next, list id, order, policy flags, 32-bit policy metadata), in a BPF arena, allocated by the `te_init` syscall program that the loader runs at load time |
| `te_lruvecs[nid]` | per NUMA node: a `bpf_spin_lock`, `TE_NR_LISTS` list heads (head = newest, tail = oldest, with folio and page counts) and the policy's `struct te_extra` |
| `te_push_head/tail`, `te_unlink`, `te_replace` | O(1) list operations, called with the node's lock held |
| `te_nominate(ctx, lv, pfn, n)` | unlink and hand over one victim; returns its pages, 0 when the batch is full |
| `te_forget`, `te_migrate` | default handlers for `folio_removed`/`folio_evicted` and `folio_migrated` |
| `te_pick_type()` | the kernel's anon/file split: its own scan targets on the classic engine (they include its IO cost model), swappiness × size on MGLRU |
| `te_order()`, `te_type()`, `te_may()`, `te_returned()` | small helpers |
| `TE_POLICY_INIT` | optional policy function run by `te_init` to allocate more arena memory (LHD's tables) |
| `te_periodic` | optional syscall program the loader runs about once a second (LHD's model rebuild) |

Why an arena indexed by pfn, rather than BPF's own lists or trees: D27.
Why engine code is organised the way it is (global functions, `bpf_for`):
D29 and [writing-policies.md](writing-policies.md).

## The engines

(The library itself is 409 lines; the line counts below are per policy.)

| Policy | Algorithm | anon | page cache | Lists per node | Lines of BPF |
|---|---|---|---|---|---|
| `lru` | the kernel's classic active/inactive LRU | yes | yes | 4 | 297 |
| `mglru` | the kernel's multi-generational LRU | yes | yes | 8 | 657 |
| `fifo` | FIFO (cache_ext) | yes | yes | 2 | 115 |
| `mru` | most recently used first (cache_ext) | – | yes | 1 | 126 |
| `s3fifo` | S3-FIFO (cache_ext, SOSP '23) | yes | yes | 4 + ghost map | 288 |
| `sampling` | LFU by sampling (cache_ext) | – | yes | 1 | 154 |
| `lhd` | Least Hit Density (cache_ext, NSDI '18) | – | yes | 1 + arena tables | 449 |

"–" means the policy leaves that type to the kernel's engine: the policy only
tracks page cache, and anon memory of the same cgroup is reclaimed by the
classic LRU or MGLRU as usual. That is a feature of the fallback design, not
a gap: a policy covers what it knows how to handle.

All engines work per node: each NUMA node has its own lists, lock and state,
and a policy attached to several nodes behaves like one independent engine
per node (demoted folios leave one node's lists and join the next one's).

### `lru`: the classic LRU

A faithful re-implementation of `mm/vmscan.c`'s two-list LRU:

* **Lists**: (anon, file) × (inactive, active). New folios go to the head of
  the inactive list, or the active list when the kernel marked them active
  (workingset refaults).
* **Accesses**: `folio_mark_accessed()`'s rule, exactly: the first access
  sets a referenced bit, the second one activates an inactive folio.
* **Aging**: `shrink_active_list()`: when `inactive_is_low()` (the kernel's
  `sqrt(10 × GB)` ratio), up to 32 folios move from the active tail to the
  inactive head.
* **Balance**: `te_pick_type()`. On the classic engine this is the kernel's
  own `get_scan_count()` result, so anon/file balance and its cost model are
  the kernel's.
* **Page table references**: the kernel checks them when it reclaims a
  nominated folio. Referenced anon comes back activated; a first-time
  referenced mapped file comes back inactive with `PG_referenced` set ("another
  trip around the inactive list"). The kernel's classic LRU makes the same
  decisions.

### `mglru`: the multi-generational LRU

A port of MGLRU's algorithm, with its own state per node:

* **Generations**: `max_seq`, `min_seq[anon/file]`, four generations per type.
  Placement of new and returned folios follows `lru_gen_folio_seq()`: faulted
  (active) folios near the young end, fresh anon one generation lower, dirty
  or under writeback kept away from the oldest, everything else in the two
  oldest generations.
* **Tiers**: `lru_gen_inc_refs()`: each `folio_mark_accessed()` bumps a
  per-folio reference count up to 4, then sets "workingset"; the tier is
  `order_base_2(refs)`, workingset is the last tier.
* **Aging**: `should_run_aging()` decides; `inc_max_seq()` first moves what is
  left in the oldest generation forward (`inc_min_seq()`, in batches, with
  `TIER_EXT_AGAIN` while it works), then bumps `max_seq` and returns
  `TIER_EXT_AGE`. The kernel's page table walk then reports young folios as
  `folio_referenced`, and the policy applies `folio_update_gen()`'s rule:
  first sighting marks the folio referenced, the second moves it to the
  youngest generation as workingset.
* **Eviction**: `sort_folio()`: from the oldest generation of the type chosen
  by the refault feedback loop (`get_type_to_scan()`), folios in a tier whose
  refault rate is higher than tier 0's (`get_tier_idx()`) are protected (moved
  up a generation), the rest are nominated.
* **Refaults**: the PID controller needs to know which evicted data comes
  back, and from which tier. The kernel encodes that in the shadow entry
  left in the page cache or swap cache. The BPF engine keeps a ghost map
  keyed by `data_key` instead (D23): tier and eviction generation of every
  evicted folio, looked up when data with the same key is added again. This
  works for anon too, because an anon folio is swapped back in from the slot
  it was evicted to.

Differences from the kernel:

| Kernel MGLRU | BPF `mglru` |
|---|---|
| lists per (generation, type, **zone**) | per (generation, type); zone restrictions are enforced by the kernel's validation, and a folio in a zone reclaim may not use comes back as RETURNED |
| refaults from shadow entries (every eviction) | ghost map of 256 Ki entries (older evictions are forgotten) |
| `NR_HIST_GENS = 1` history | same, including the carry-over averaging |
| aging walks page tables itself | asks the kernel for the walk (`TIER_EXT_AGE`); on the classic engine there is no walk, and young page table entries are only seen when a nominated folio is reclaimed (the kernel then activates it) |
| per-memcg generation balancing (`memcg_lru`) | not needed: one policy is one memcg |
| `min_ttl_ms` thrashing prevention | not implemented |
| rejected folios leave the oldest generation (`evict_folios()` marks them active) | same: a victim handed back that would land in the oldest generation is placed as active. Without this rule, read-ahead still locked for I/O was nominated over and over; page cache reclaim cost 74% more than the kernel's, 13% with it |

On the classic kernel engine `mglru` still works; it then brings MGLRU's
behaviour to a machine running the classic LRU. In the tests it keeps a
file read 8 times fully resident while a 600 MB file streams through a
200 MB cgroup; the kernel's classic LRU keeps 0% of it.

### The cache_ext ports

cache_ext's policies were written against its kernel list API
(`bpf_cache_ext_list_add/move/del/iterate/sample`) and applied to the page
cache of files in a watched directory. The ports use the engine library
instead and apply to a whole cgroup on a node.

* **`fifo`**: one FIFO per type. Accesses do not matter, and nominated folios
  are reclaimed even when referenced (`folio_reclaim` answers `RECLAIM` for
  `TIER_EXT_RC_NOMINATED`), which is what cache_ext achieved by reclaiming
  with `reclaim_pages()`.
* **`mru`**: evicts from the newest end. Anon is left to the kernel (MRU is
  pathological for anon). This is the policy that candidate filtering could
  not express at all: in the test, a file read once that does not fit keeps
  82% of its first third in memory; the kernel keeps 0% of it.
* **`s3fifo`**: small FIFO S (10%), main FIFO M, ghost queue G, per type.
  Ghosts are keyed by `data_key`, so S3-FIFO works for anon too, which
  cache_ext's could not. Two adaptations, both found by testing:
  * **large folios**: a single sequential `read()` pass can mark a 2 MiB
    folio accessed more than once (a read spanning folios marks the later
    ones; a later read may mark the same folio again). S3-FIFO promotes on
    "accessed more than once", so a scan flooded M and evicted the hot data.
    Accesses to a folio less than 1 ms apart now count once. cache_ext ran on
    a kernel whose page cache used 4 KiB folios.
  * **bounded work**: M's eviction loop may have to give many folios a
    second chance before one is evictable. The port answers
    `TIER_EXT_AGAIN` instead of falling back (D25).
* **`sampling`**: LFU by sampling. For each victim, 20 folios are taken from
  the old end; the least frequently used is nominated and the others move to
  the new end (the semantics of `bpf_cache_ext_list_sample()`).
* **`lhd`**: Least Hit Density. Per-class hit and eviction histograms by age,
  a hit-density model rebuilt every 2^20 requests, and victims chosen as the
  lowest density of a 16-folio sample. The tables (6 MiB) and per-pfn
  metadata live in the arena. cache_ext's `app` dimension was always the same
  value, so only the 16 classes it actually used are kept. The model rebuild
  is too long for a reclaim callback; cache_ext's loader ran it when a ring
  buffer event arrived, here the loader runs `te_periodic` every second and
  it rebuilds when 2^20 requests have passed.

Not ported: cache_ext's `get_scan` policy, which is specific to a LevelDB
GET/SCAN benchmark (it recognises the benchmark's scan threads); and its
per-directory "watch list", which tier_ext replaces with per-cgroup
attachment.

## What the ports taught about the interface

These are the gaps and surprises found while porting, i.e. the most useful
output of phase 2 (each fix is a decision in [decisions.md](decisions.md)):

1. **The output of `pick_victims` could not be a writable context array**:
   the verifier forbids variable-offset writes through BTF pointers, so it
   became a kfunc (D20).
2. **Every nominated folio needs a way back**: without the RETURNED hand-off,
   a folio the kernel could not reclaim right now would be lost to the policy
   or re-nominated forever (D21).
3. **Ghost lists need an identity that survives eviction**: pfns do not;
   `data_key` does, for anon too (D23).
4. **Bounded work per call**: algorithms that rotate before they find a
   victim need `TIER_EXT_AGAIN` (D25), or the stale fallback evicts
   read-ahead (D22).
5. **Page table access information needs aging on demand**: MGLRU only walks
   page tables when it needs a new generation; an engine (or `lfu`) asks with
   `TIER_EXT_AGE` (D25, D26).
6. **Frequency on large folios** is not frequency on pages: a folio can
   collect several "accesses" from one sequential pass (the `s3fifo` note
   above). A policy that counts accesses needs to decide what one access is.
   The kernel does not help here; a timestamp or offset in the access event
   would be a possible future extension.
7. **Stale pfns come from folios freed while isolated**; one extra
   `folio_removed` site closed the last source (D24).
8. **Verifier limits shape engine code** (D29).
