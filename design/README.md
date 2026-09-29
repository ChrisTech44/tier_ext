# tier_ext design

tier_ext lets a small BPF program decide how memory is reclaimed for **one
cgroup on one NUMA node at a time** (one *lruvec*), while every other part of
the system keeps using the kernel's normal policy.

If you have never looked at Linux memory reclaim, read the documents in this
order. Each one is short and assumes only the ones before it.

| # | Document | What it answers |
|---|---|---|
| 1 | [background.md](background.md) | What are reclaim, LRU lists, MGLRU, memcgs, lruvecs, memory tiers and demotion? |
| 2 | [goals.md](goals.md) | What problem does tier_ext solve, and what is deliberately out of scope? |
| 3 | [architecture.md](architecture.md) | How is it built? Where does the kernel call the policy, and what can the policy answer? |
| 4 | [safety.md](safety.md) | Why can't a buggy or hostile policy crash the machine or make memory unreclaimable? |
| 5 | [decisions.md](decisions.md) | Every design decision, the alternatives we considered, and why we chose what we did. |
| 6 | [engines.md](engines.md) | Policies that choose every victim: LRU, MGLRU and cache_ext's policies re-implemented in BPF, for anon and page cache. |
| 7 | [writing-policies.md](writing-policies.md) | How to write, build and attach your own policy. |
| 8 | [testing.md](testing.md) | How it was tested, and the measured results. |
| 9 | [limitations.md](limitations.md) | Known limitations and future work. |

## One-paragraph summary

A policy is a BPF `struct_ops` object of type `struct tier_ext_ops`. It names a
cgroup (`cgroup_id`) and a set of NUMA nodes (`node_mask`). When it is attached,
the kernel stores a pointer to it in each selected lruvec. From then on, when
reclaim works on one of those lruvecs it asks the policy at four decision
points (skip this lruvec? which anon/file balance? keep or evict this folio
while scanning? keep, evict, demote this folio now?) and tells it about three
events (folio added, accessed, evicted). Every question has a "default" answer,
which means "do exactly what the kernel would have done", and every lruvec
without a policy is untouched. The kernel ignores "keep" and "skip" answers once
reclaim becomes desperate (priority 0), so a policy can shape reclaim but can
never prevent it.

A policy can also **choose the victims itself** (`pick_victims`): it keeps its
own ordering of the lruvec's folios, fed by events (added, accessed, evicted,
removed, migrated, and page table accesses found by MGLRU), and hands the
kernel page frame numbers, which the kernel re-validates before reclaiming
them. With that, complete reclaim algorithms run per lruvec in BPF: the
kernel's classic LRU and MGLRU re-implemented for anon memory and page cache,
and ports of cache_ext's FIFO, MRU, S3-FIFO, LFU and LHD policies. Whatever a
policy does not cover is still reclaimed by the kernel's engine.

## Where the code is

| Path | What |
|---|---|
| `linux/include/linux/tier_ext.h` | The policy interface: `struct tier_ext_ops`, context structs, verdicts |
| `linux/mm/tier_ext.c` | struct_ops registration, attach/detach, hook dispatch, debugfs |
| `linux/mm/vmscan.c` | The reclaim hook sites (search for `tier_ext`) |
| `linux/mm/folio.c` | The `folio_added` / `folio_accessed` / `folio_removed` event sites |
| `linux/mm/migrate.c`, `linux/mm/huge_memory.c` | The `folio_migrated` and split-tail `folio_added` sites |
| `linux/mm/Kconfig` | `CONFIG_TIER_EXT` |
| `policies/` | Loader (`tier_ext_load`), example policies, and the engine library `te_engine.bpf.h` |
| `tests/` | VM test suite |
