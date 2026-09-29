# Design decisions

Each entry gives the decision, the alternatives considered, and the reasoning.
They are numbered so other documents and code comments can refer to them.

---

## D1. Policies are BPF struct_ops programs

**Decision.** A policy implements `struct tier_ext_ops`, a new BPF struct_ops
type.

**Alternatives.**
* *A kernel-module ops table.* As expressive, but a buggy module crashes the
  kernel, and modules are hard to deploy in production.
* *Attaching `fmod_ret`/kprobe programs to vmscan functions.* No kernel change
  needed, but the functions are `static` and often inlined, their semantics
  change between releases, and return-value overriding is not allowed for most
  of them.
* *More sysfs/cgroup knobs* (per-node swappiness, and so on). Safe but not
  programmable: each new idea needs a new knob.

**Why.** struct_ops is the established way to implement a kernel ops table in
BPF (TCP congestion control, sched_ext, HID-BPF). It gives verifier safety,
typed arguments via BTF, load/unload at run time, and a stable, documented
interface that is independent of vmscan internals.

---

## D2. The policy pointer lives in `struct lruvec`

**Decision.** Add `struct tier_ext_policy __rcu *tier_ext` to `struct lruvec`.

**Alternatives.** A pointer per memcg, plus a node mask inside the policy. Or a
global hash table keyed by (memcg, node).

**Why.** The lruvec is exactly the unit the vision asks for, and it is the unit
reclaim already iterates over, so every hook has the lruvec at hand and needs a
single pointer load. A per-memcg pointer would need an extra node-mask test and
could not express "memcg A uses policy X on node 0 and policy Y on node 1". A
hash table costs a lookup on every folio in hot paths. The price is 8 bytes per
lruvec, and only with `CONFIG_TIER_EXT=y`.

---

## D3. Policies filter the kernel's candidates rather than choose victims

**Decision.** The kernel's scanners still walk the lists and present folios;
the policy returns a verdict for each one (`folio_scan`, `folio_reclaim`). The
policy never hands the kernel a folio to evict.

**Alternative: victim selection (the cache_ext model).** The policy keeps its
own lists of folios in BPF data structures and, when the kernel needs memory,
iterates them and nominates victims. cache_ext does this for page cache.

**Why.**
* **Safety.** Nominating a folio means BPF holds a folio pointer across time.
  Keeping that safe requires shadow data structures kept in sync with every
  folio free, truncation and migration (cache_ext maintains a hashtable of
  valid folios for this), plus verifier changes for callback kfuncs. Verdicts on
  folios the kernel already holds under the LRU lock or the folio lock need
  none of that.
* **Both engines, anon and file.** Candidate filtering plugs into the classic
  LRU and MGLRU at their existing decision points and covers anon too. The
  cache_ext approach covers page cache and replaces the engine rather than
  building on it.
* **Fallback is natural.** "No opinion" just means "let the engine decide".
  With victim selection, fallback means switching between two list structures.

**Cost.** A policy cannot evict a folio the scanner has not reached yet, and
expressing "evict the newest first" (MRU) is only approximate. Frequency-,
type-, tier- and age-based policies map well (see `lfu`, `stream`,
`tier_demote`).

**Update (phase 2).** Victim selection was added later as an *extra*,
optional callback (`pick_victims`, D19-D22), designed so that the safety
arguments above still hold: the policy names page frame numbers, never
pointers, and the kernel re-validates each one. Filtering stays the default
mode; a policy may use either or both.

---

## D4. The target is written into the struct_ops itself

**Decision.** `cgroup_id` and `node_mask` are data members of `struct
tier_ext_ops`, set by user space before attaching. Attaching the struct_ops
map (normally via a BPF link) attaches the policy.

**Alternatives.** A new `BPF_LINK_TYPE` taking a cgroup fd; a cgroup file such
as `memory.tier_ext` taking a map id; a sysfs file per node.

**Why.** This needs no new UAPI (syscall commands, link types, cgroup files).
HID-BPF (`hid_id`) and sched_ext (`name`, `timeout_ms`, ...) use the same
pattern. A single object fully describes "this policy for these lruvecs", and
the same compiled policy can be attached to many targets by changing two
fields (the loader does this with `-c` and `-n`). To retarget, you detach and
re-attach, which takes milliseconds.

---

## D5. A policy applies to exactly one memcg, not its descendants

**Decision.** Exact match: attaching to `/sys/fs/cgroup/a` does not affect
`/sys/fs/cgroup/a/b`. Folios charged to `a/b` live on `a/b`'s lruvecs.

**Alternative.** Inherit: descendants without their own policy use the nearest
ancestor's.

**Why.** Exact match is what "per-lruvec" means, it is simple to reason about,
and it needs no work at cgroup creation time. Inheritance would need either a
walk up the hierarchy on every hook (cost in hot paths) or pointer propagation
on cgroup creation, attach and detach (complexity and races). Most containers
put their processes in a leaf cgroup, and a policy can be attached to each
leaf. If inheritance is needed later, it can be an opt-in `flags` bit.

---

## D6. At most one policy per lruvec; no stacking

**Decision.** Attaching a policy to an lruvec that already has one fails with
`EBUSY`.

**Why.** Reclaim decisions do not compose: if one policy says KEEP and another
says RECLAIM, any combination rule is arbitrary. A policy that wants to combine
behaviours can do so in its own code. Different lruvecs of the same memcg (other
nodes) and the same node of other memcgs can have different policies.

---

## D7. Hooks sit at engine-neutral decision points, in both engines

**Decision.** Four decision hooks, each placed where *both* the classic LRU and
MGLRU make the equivalent decision:

| Hook | classic | MGLRU |
|---|---|---|
| `lruvec_shrink` | `shrink_lruvec()` | `try_to_shrink_lruvec()` |
| `swappiness` | `get_scan_count()` | `get_swappiness()` |
| `folio_scan` | `isolate_lru_folios()` | `sort_folio()` |
| `folio_reclaim` | `folio_check_references()` in the shared `shrink_folio_list()` | same |

**Why.** The vision requires falling back to "the default (LRU, MGLRU)", so a
policy must work whichever engine is running, including after a run-time
switch. Choosing points that exist in both engines lets one policy work on
both. `ctx->engine` tells the policy which one is active when it matters.

---

## D8. `DEFAULT` is 0, and everything unexpected means `DEFAULT`

**Decision.** `TIER_EXT_DEFAULT = 0`. Missing callbacks, unknown return values
and invalid flag combinations are all treated as `DEFAULT` (and counted in
`invalid`).

**Why.** A zero-initialized or half-written policy behaves like no policy.
Fallback is the norm, so "I don't know" must be the cheapest thing to say. The
`invalid` counter keeps mistakes visible instead of silent.

---

## D9. Restrictive verdicts are ignored at reclaim priority 0

**Decision.** `KEEP`, `SKIP` and `DEMOTE_ONLY` are honoured only while
`sc->priority > 0`.

**Alternatives.** Trust the policy completely (a buggy policy then causes OOMs
or hangs). A per-policy budget of KEEPs. Detach the policy automatically when
it misbehaves (sched_ext's approach).

**Why.** Priority 0 is the kernel's own definition of "desperate": every list
is scanned in full. Honouring the policy until then gives it complete control
in normal operation, while guaranteeing reclaim never does worse than the
kernel alone would under real pressure. It needs no tuning and no watchdog.
Auto-detaching was rejected because a policy that is right 99% of the time
should not disappear because of one burst of pressure.

---

## D10. Two per-folio decisions: at scan time and at reclaim time

**Decision.** `folio_scan` (folio still on the list, LRU lock held) and
`folio_reclaim` (folio isolated, locked, references counted) are separate
callbacks.

**Why.** They see different information and have different power. At scan time
the policy can stop MGLRU's list-level protection from saving a folio, or save
it before any expensive work (rmap walk, unmapping) is done. At reclaim time
the page-table reference count is known and demotion can be controlled. A
single hook would lose one or the other. A policy may implement either or both;
simple policies usually need only `folio_reclaim`, which works identically in
both engines.

---

## D11. What `KEEP` means in each engine

**Decision.**
* MGLRU, `folio_scan`: move the folio to the **youngest generation** (new helper
  `folio_youngest_gen()`), as if just accessed.
* Classic, `folio_scan`: rotate to the **head of the list being scanned**.
* Both, `folio_reclaim`: return it to the LRU **as active** (the kernel's own
  "activate" path).

**Alternatives.** For MGLRU, `folio_inc_gen()` (the next generation only); for
classic, move to the active list during the scan.

**Why.** "KEEP" should mean "treat as recently used" in the engine's own terms.
In MGLRU the next generation is the next one evicted, so it gives almost no
protection; the youngest generation does. For classic, moving to another list
inside `isolate_lru_folios()` would mean doing LRU size accounting in the middle
of a batch that the function accounts at the end; rotating within the list is
the move that function already makes for busy folios, so it is known to be
safe. Activation is available through `folio_reclaim`.

---

## D12. Tiering control is "demote or not", not "demote where"

**Decision.** Flags on the `folio_reclaim` verdict: `NO_DEMOTE` (free instead of
demoting) and `DEMOTE_ONLY` (demote or keep, never free).

**Why.** Whether a folio is worth keeping in slower memory (anon usually is;
clean page cache often is not) is a per-workload, per-tier judgement, and
exactly what per-lruvec policies are for. Choosing the target node interacts
with the allocator's fallback lists and the memory-tier hierarchy, and needs a
different mechanism (a folio can be demoted in a batch to one target). With
`DEMOTE_ONLY` on the last tier, where demotion is impossible, the result is
"keep in memory", which gives "anon never leaves RAM, it only moves down
tiers" when attached to all nodes (verified in testing).

---

## D13. Kernel state is passed as read-only snapshots

**Decision.** Callbacks receive small context structs filled by the kernel,
plus a read-only `struct folio *`. There are no kfuncs.

**Alternatives.** Pass raw `lruvec`/`scan_control` pointers; export kfuncs to
query or modify state.

**Why.** `struct scan_control` is private to vmscan.c and changes often. A
snapshot struct is a stable contract that is cheap to fill and easy to
document. Read-only access means a policy can only act through verdicts, which
the kernel validates (D8, D9). kfuncs can be added later if policies need
information that is expensive to compute for every call.

---

## D14. Events: `folio_added`, `folio_accessed`, `folio_evicted`

**Decision.** Hook `lru_add()` (folio enters the LRU through the per-CPU
batches), `folio_mark_accessed()`, and the free path of `shrink_folio_list()`.

**Alternatives.** Hook `lruvec_add_folio()`, which also fires on every
internal list move such as activation. Report every removal from the LRU.

**Why.** Policies that keep per-folio metadata need "a folio arrived", "it was
used", "it is gone". `lruvec_add_folio()` would also fire for moves between
lists, which is noise. Removals other than reclaim (truncation, unmap, process
exit) happen in many places; hooking them all would be invasive. Policies
should store metadata in an LRU hash map so that entries for folios that
vanished without an eviction event age out (as `lfu` does).

---

## D15. Counters in debugfs

**Decision.** Per-policy, per-CPU counters for every call and verdict, shown in
`/sys/kernel/debug/tier_ext`.

**Why.** The first question when a policy "does nothing" is whether it is being
called at all, and what the kernel did with its answers. Per-CPU counters cost
almost nothing. debugfs keeps them out of the stable ABI.

---

## D16. Kconfig: `TIER_EXT` depends on BPF JIT, BTF and MEMCG

**Why.** struct_ops needs the BPF JIT (trampolines) and BTF. Targeting is by
cgroup id, so memcg is required. (When memcg is compiled in but disabled at
boot, only `cgroup_id = 0` is accepted, and it targets the per-node lruvecs.)

---

## D17. Repository layout

**Decision.** `linux/` is a git repository of its own, nested in the tier_ext
repository. The outer repository records its commit (a gitlink), and also
holds `design/`, `policies/` and `tests/`.

**Why.** The outer repository already recorded `linux` as a gitlink, but the
directory had no git metadata, so kernel changes could not be committed. A
nested repository with an "unmodified 7.3-rc5" baseline commit keeps every
kernel change reviewable with `git -C linux log -p`, without pushing 1.8 GB of
kernel source into the outer repository.

---

## D18. Test environment

**Decision.** Test in the LinuxDev QEMU VM with `configs/hmat.conf` (node 0: 4
CPUs + 4 GB "DRAM"; node 1: 8 GB CPU-less "far memory" in a lower tier), with
zram swap set up by the test suite.

**Why.** Per-node policies and demotion are only meaningful with two nodes in
different tiers. zram makes anon memory reclaimable without a swap disk.
Snapshot mode keeps every run starting from the same disk.

---

## D19. Victims are nominated as page frame numbers, and re-validated

**Decision.** A new callback, `pick_victims`, lets a policy nominate up to 32
folios per call by **pfn** (the page frame number of the folio's first page).
The kernel trusts nothing about a pfn. For each one it checks, in order:
the pfn is online memory (`pfn_to_online_page()`), a reference can be taken
on a head page (`folio_get_nontail_page()`, which fails for free and tail
pages), the folio is on an LRU list, and that LRU belongs to *this* lruvec
(`folio_lruvec()`). Only then does it isolate the folio (`folio_isolate_lru()`)
and reclaim it through the usual `shrink_folio_list()`.

**Alternatives.**
* *Nominate `struct folio *` pointers* (cache_ext). BPF cannot be trusted with
  a pointer that outlives the moment it was handed out: the folio may be freed
  and the memory reused. cache_ext therefore keeps a kernel-side hash table of
  "valid folios" updated on every insertion and removal, plus verifier changes
  for its list kfuncs.
* *Kernel-managed lists* that BPF manipulates through kfuncs (cache_ext's
  `bpf_cache_ext_list_*`). Convenient, but it puts a second set of per-folio
  list nodes, and their locking, into the kernel.

**Why.** A pfn is just a number: BPF holds no kernel pointers, needs no
verifier changes, and a stale or malicious pfn is harmless because validation
is complete (the same checks compaction uses to take pages it does not own).
The price is that policies keep their own orderings keyed by pfn and must
follow folios that are freed or migrated (D24); the `garbage` test shows that
getting this wrong costs only efficiency, never safety.

---

## D20. Victims are written through a kfunc, not through the context

**Decision.** The policy adds victims with the kfunc
`bpf_tier_ext_add_victim(ctx, pfn)`, which appends to the context's array and
returns `-ENOSPC` when it is full. The context stays read-only to BPF.

**Alternative.** Make `ctx->nr` and `ctx->pfns[]` writable in
`btf_struct_access()` (the pattern HID-BPF uses for its output fields).

**Why.** The verifier only allows *constant* offsets when writing through a
BTF pointer, so `ctx->pfns[n++] = pfn` with a variable `n` is rejected; a
policy would have to unroll every write. The kfunc is two lines of kernel
code, bounds-checks itself, is marked `KF_SPINLOCK_SAFE` so it can be called
while the policy holds its own list lock, and is restricted by a kfunc filter
to `tier_ext_ops.pick_victims` programs.

---

## D21. A nomination is a hand-off

**Decision.** A policy removes a folio from its own structures when it
nominates it. From then on the folio belongs to the kernel until it is either
evicted (`folio_evicted`) or handed back through `folio_added` with the flag
`TIER_EXT_F_RETURNED`. Folios are handed back when reclaim did not free them
(locked, under writeback, referenced, ...) and when they were valid but not
reclaimable in this pass (wrong zone, anon without swap, ...). The flags in
the context say what the kernel decided, e.g. `TIER_EXT_F_ACTIVE` when it
activated the folio because its page table entries were young.

**Alternative.** Leave nominated folios in the policy's lists, and add a
callback reporting each nomination's outcome.

**Why.** Stale pfns heal themselves: a pfn that no longer belongs to the
lruvec is nominated once, rejected, and gone from the policy. Survivors come
back with the kernel's verdict attached, which is exactly the information a
policy needs to place them (e.g. the classic LRU's "referenced: activate").
And there is no extra callback: survivors look like any other folio being
added to the LRU, which they are.

---

## D22. Victims come first, the engine covers the rest

**Decision.** Before the engine (classic or MGLRU) scans an lruvec, the kernel
asks the policy for victims, in batches, until the engine's planned scan
budget for this pass is used up, the reclaim target is met, or the policy has
nothing (valid) left. The pages the victims consumed are taken out of the
engine's budget, and the engine reclaims whatever is left, as before. At
priority 0 the engine always gets its full budget. Nominated folios still go
through the normal reference check, so `folio_reclaim` applies to them too;
its context carries `TIER_EXT_RC_NOMINATED`, and a policy that wants its
victims reclaimed regardless of references answers `RECLAIM` (FIFO and MRU
do).

**Alternative.** Let the policy replace the engine completely for its lruvecs
(cache_ext's model: its page cache eviction does not fall back).

**Why.** The engine as a backstop keeps two guarantees: a policy that
nominates nothing (or only garbage) makes reclaim no worse than without it,
and a policy can never *reduce* reclaim, since nominating only adds work the
kernel would otherwise have done itself. It also allows partial policies: a
page-cache-only policy (`mru`, `sampling`, `lhd`) leaves anon to the engine.

**Caveat learned in testing.** The fallback reclaims by the engine's own
ordering, and that ordering is stale while a policy does the evicting (for
MGLRU, the generations stop advancing). The first S3-FIFO port under-nominated
and the fallback evicted read-ahead before it was read: tens of thousands of
refaults on a single sequential read. Fallback is a safety net, not a
performance path; engine policies should cover their whole budget, which is
why D25 exists.

---

## D23. A `data_key` identifies the data, independent of the page frame

**Decision.** Folio contexts carry `data_key`: for page cache a hash of
(device, inode) in the upper 32 bits and the file index in the lower 32; for
swap cache `TIER_EXT_KEY_SWAP | swap entry`; 0 if unknown. For
`folio_evicted` the kernel computes it just before the folio leaves the page
or swap cache, since the identity is lost afterwards.

**Why.** Ghost lists (S3-FIFO's G, MGLRU's refault tiers, ARC, LIRS) need to
recognise data that comes *back* after eviction, and the pfn cannot do that:
the data returns in a different page frame. cache_ext keyed ghosts by
`(mapping->host, index)`, which covers page cache only. The swap slot makes
the same idea work for anon memory: an anon folio is evicted from a swap slot
and swapped back in from the same slot. The hash can collide; ghost entries
are hints, so that only costs accuracy.

---

## D24. Lifecycle events follow folios the policy may be tracking

**Decision.** Three new events and one extension:

| Event | Fired from | Why |
|---|---|---|
| `folio_removed` | `__page_cache_release()` (freed while on the LRU) and `move_folios_to_lru()` (the last reference went away while isolated) | truncation, unmap, exit: the pfn no longer holds this data |
| `folio_migrated` | `migrate_folio_move()`, before the new folio goes on an LRU | compaction, NUMA balancing, demotion: `old_pfn` became `pfn` |
| `folio_referenced` | `walk_update_folio()` (MGLRU page table walks and look-around) | accesses through page tables (D26) |
| `folio_added` + `TIER_EXT_F_SPLIT` | `lru_add_split_folio()` | the tails of a split large folio are new folios on the LRU |

`folio_migrated` goes to the policy of the *old* lruvec, so it can forget the
old pfn or, when the folio stayed on the same node (compaction), give the new
pfn the old one's place (`te_migrate()` in the engine library). The new folio
is also reported to its own lruvec through `folio_added`.

**Why.** Stale pfns are safe (D19), but each costs a wasted nomination. The
second `folio_removed` site was found by testing: folios freed while isolated
were the only remaining source of stale entries (1 invalid nomination per
~400,000 before the fix, 0 after).

---

## D25. `TIER_EXT_AGE` and `TIER_EXT_AGAIN`: a policy can ask for more

**Decision.** `pick_victims` returns flags:

* `TIER_EXT_AGE`: after this batch, run the engine's aging for this lruvec
  (MGLRU: `try_to_inc_max_seq()`, i.e. a page table walk). At most once per
  lruvec per reclaim pass; ignored on the classic engine
  (`TIER_EXT_VF_CAN_AGE` tells the policy whether it is available).
* `TIER_EXT_AGAIN`: "I found no victim in this batch but made progress (moved
  folios between my queues); call me again". Honoured for at most 16
  consecutive batches without victims.

**Why.** Engine policies must do bounded work per call (the verifier demands
bounded loops, and the lock they hold disables interrupts), but some
algorithms need unbounded rotation before a victim appears: S3-FIFO's main
queue decrements frequencies until one reaches 0, MGLRU moves the oldest
generation forward before it can create a new one. Without AGAIN the kernel
would fall back to its stale ordering (D22). AGE lets an engine policy get
the page table access information that only the kernel's walk can produce.
A filter policy can use it too: `lfu` implements `pick_victims` only to
request aging every 200 ms and nominates nothing.

---

## D26. Page table accesses are reported where MGLRU finds them

**Decision.** `folio_referenced` fires in `walk_update_folio()`, the single
function both MGLRU's page table walk and its rmap look-around call for each
folio whose PTEs were young. It fires for every young folio of a policed
lruvec, before MGLRU updates the folio's generation.

**Alternatives.** Hook `folio_referenced()` (rmap), which every reclaim path
calls for each candidate; or add a separate walker for BPF.

**Why.** rmap reference counts are already delivered to `folio_reclaim`
(`referenced_ptes`). What policies lacked was a *frequency* signal for anon
and mapped files between reclaim attempts, and MGLRU's walk is the kernel's
efficient way to collect exactly that (it batches, uses bloom filters to skip
cold page tables, and runs when aging). The hook costs one pointer test per
young folio when a policy is attached anywhere, and nothing otherwise (static
key). On the classic engine there are no walks, so no events.

---

## D27. Engine policies keep one 16-byte node per page frame in a BPF arena

**Decision.** `policies/te_engine.bpf.h` gives policies doubly linked lists of
pfns. Nodes live in a BPF arena, indexed directly by pfn (16 bytes per 4 KiB
page, 0.4% of memory, allocated once at load by the `te_init` syscall
program). Links store `pfn + 1` so that zeroed memory is an empty list. Each
node (= lruvec, per policy) has a `bpf_spin_lock`, its list heads and the
policy's own state in an array map. Every list operation is O(1).

**Alternatives.**
* *`bpf_list_head`/`bpf_rb_root` with `bpf_obj_new()` nodes.* Each folio
  needs an allocation (in reclaim context, where it may fail), a pfn-to-node
  hash map, and refcounted kptrs to find a node again for an event. All
  doable, but slow and verifier-heavy.
* *A BPF array map indexed by pfn.* Array values are limited in size, and map
  lookups (helper calls) are not allowed while holding a `bpf_spin_lock`,
  which list surgery on neighbours needs.
* *Kernel-provided lists via kfuncs* (cache_ext). See D19.

**Why.** Arena memory is plain memory to a BPF program: loads and stores are
allowed under the lock, there is no per-folio allocation, and the index is the
pfn itself, so every event finds its node in O(1). Arena accesses are safe
even when wrong: an address outside the allocated pages faults into the BPF
exception handler (reads return 0), never into the kernel.

---

## D28. BPF engines are an overlay on the kernel's lists

**Decision.** With an engine policy (`lru`, `mglru`, the cache_ext ports)
attached, the kernel keeps maintaining its own LRU lists or MGLRU
generations exactly as before; the policy's lists are a second ordering on
top, used to choose victims.

**Alternative.** Take the lruvec's folios off the kernel's lists and let the
policy own them.

**Why.** Every kernel path that isolates, moves, mlocks, migrates or frees a
folio keeps working unmodified, and fallback (D22, priority 0, detach) is
instant because the kernel's structures were never abandoned. The cost is
double bookkeeping, and the kernel's ordering goes stale (D22).

---

## D29. Verifier-friendly structure for engine policies

**Decision.** Engine policies put each locked loop in a *global* BPF function
(`__noinline`, non-static, `__arg_trusted` for the context), use `bpf_for()`
open-coded iterators (spin-lock-safe), and never count in a loop-carried local
variable (they count in map memory instead).

**Why.** Found while writing `lru`. Static inline code is re-verified for
every path that reaches it, so two locked loops in sequence, or one after a
branchy helper, exceeded the verifier's limit of 8192 pending branches. A
global function is verified once, on its own. `bpf_for()` lets the verifier
prove a loop converges instead of unrolling it. And a local counter that goes
0, 1, 2, ... makes every iteration's state different, so the loop never
converges; the same counter in memory is an unknown value to the verifier. An
unrolled bit-by-bit integer square root failed for the same reason (2^16
paths); a bounded search replaced it. All of this is in
[writing-policies.md](writing-policies.md).

---

## D30. Policies can be replaced in place

**Decision.** `tier_ext_ops` implements the struct_ops `.update` operation, so
`BPF_LINK_UPDATE` (libbpf `bpf_link__update_map()`; the loader's `-u NEXT.bpf.o`
plus `SIGUSR1`) swaps the programs of an attached policy without detaching
it. The new struct_ops must name the same cgroup and nodes. Every dispatch
site reads the callback it calls exactly once (`TE_OP()`), and the update
waits for an RCU grace period, so no old program runs after it returns.

**Why.** Detach-then-attach leaves a window in which the default policy runs;
for an engine it also means the new policy starts on a cgroup whose folios
it has not seen. An in-place swap removes the window. It does not transfer
state: the new policy has its own maps and learns folios as they are added
(what the old one knew is reclaimed by the engine's fallback). Retargeting
stays a detach and an attach.
