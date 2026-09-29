# Goals and non-goals

## The problem

Every reclaim policy in mainline Linux is **global**. The classic LRU and MGLRU
apply one algorithm to every lruvec. Their knobs are global too
(`vm.swappiness`, `lru_gen/min_ttl_ms`, `demotion_enabled`), or per cgroup at
best, and never per node. On cgroup v2 there is not even a per-cgroup
swappiness.

Research systems that make reclaim programmable (cache_ext, and proposals like
tier_bpf or reclaim_ext) replace or tune the policy, but it is still **one
policy for everyone**: a program chosen for a database hurts a batch job sharing
the machine, and a policy good for DRAM may be wrong for a slow CXL node.

Workloads and tiers differ, though:

* A streaming backup job should recycle its own page cache quickly. A database
  on the same host should keep its cache.
* On the DRAM node, clean page cache is better dropped than demoted, because
  re-reading it is cheap and it would only pollute the slow tier. Anon memory
  is better demoted than swapped. On the slowest node the trade-off is
  different again.
* A latency-critical service may deserve protection on its DRAM node but not
  on its far-memory node.

## Goals

1. **Per-lruvec policies.** A policy controls exactly the (memcg, node) pairs it
   was attached to. Different lruvecs can run different policies at the same
   time.
2. **Default fallback.** Anything a policy does not decide, and every lruvec
   without a policy, behaves exactly as without tier_ext, whether the kernel
   runs the classic LRU or MGLRU.
3. **Expressive enough to matter.** Policies can influence the decisions that
   matter for reclaim quality: whether to reclaim an lruvec at all, anon/file
   balance, which folios to keep or evict, and demotion versus eviction.
4. **Safe.** A buggy or hostile policy must not crash the kernel, corrupt the
   LRU state, or make memory unreclaimable (cause OOMs or hangs that would not
   otherwise happen).
5. **Cheap when unused.** With no policy attached, the cost is a few
   never-taken branches (static keys).
6. **Dynamic.** Policies attach and detach at run time without rebooting, and
   detaching restores the default immediately.
7. **Understandable.** Small interface, documented semantics, observable
   through debugfs counters.

## Non-goals (for now)

* **Arbitrary victim selection.** The policy judges the folios the kernel's
  scanner presents (the cold end of the lists). It cannot say "evict that
  specific folio over there". See [decisions.md](decisions.md), D3.
* **Replacing aging.** How MGLRU creates generations, and how the classic LRU
  deactivates active folios, is unchanged.
* **Choosing the demotion target.** A policy can forbid or require demotion,
  but the kernel still picks which slower node receives the folio.
* **Promotion** (moving hot folios up to a faster tier) is a NUMA-balancing
  decision, not a reclaim decision, and is out of scope.
* **Hierarchical policies.** A policy applies to exactly one memcg, not to its
  descendants. See D5.
