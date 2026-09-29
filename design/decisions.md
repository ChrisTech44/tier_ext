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
`tier_demote`). Victim selection could be added later as an extra callback,
see [limitations.md](limitations.md).

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
