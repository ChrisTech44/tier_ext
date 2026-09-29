# Limitations and future work

## Current limitations

* **Candidate filtering only** (D3). A policy judges the folios the scanner
  presents; it cannot nominate arbitrary victims. MRU-like policies
  ("evict the newest first") can only be approximated.
* **No inheritance** (D5). A policy covers exactly one memcg. Folios charged to
  child cgroups are on the children's lruvecs.
* **Aging is not programmable.** When MGLRU creates generations, and how the
  classic LRU deactivates active folios (`shrink_active_list()` still calls
  `folio_scan`, but only KEEP has an effect there), stays with the kernel.
* **No per-lruvec scan pressure.** A policy can skip an lruvec or change its
  anon/file balance, but not scale how many pages are scanned in it.
* **Demotion target is fixed.** Policies can forbid or require demotion but not
  pick the node.
* **Page-table accesses are not events.** Only `folio_mark_accessed()` (system
  call level access) is reported. Accesses through mappings show up as
  `referenced_ptes` when reclaim looks at a folio, and in MGLRU generations.
* **Slab shrinkers are untouched.** tier_ext affects LRU folios only; the
  per-memcg slab shrinking that follows each lruvec is unchanged.
* **Node mask limited to 64 nodes** (`u64 node_mask`).
* **DEMOTE_ONLY and large folios.** On a kernel without THP migration, large
  folios cannot be demoted, so DEMOTE_ONLY keeps them until priority 0.

## Future work

1. **Victim nomination.** A `pick_victims` callback taking a small array the
   policy fills with pfns. The kernel would re-validate each pfn (still on this
   lruvec, same folio, isolatable) under the LRU lock before using it. This
   gives cache_ext-style freedom without trusting BPF-held pointers.
2. **Optional inheritance.** A `TIER_EXT_F_DESCENDANTS` flag, with the pointer
   propagated at cgroup creation and at attach time.
3. **Scan scaling.** Let `lruvec_shrink` return a scaling factor for
   `nr_to_scan`, not just skip/default.
4. **kfuncs for expensive information** (e.g. refault distance from the
   workingset shadow entries, or per-lruvec refault rates), computed on demand
   instead of in every context.
5. **Demotion target choice.** Let `folio_reclaim` return a preferred node among
   the allowed demotion targets.
6. **Link update** (`.update`), to swap a policy atomically without a gap in
   which the default policy runs.
7. **Benchmarks.** Measure the example policies on real workloads (YCSB,
   XSBench, which LinuxDev already has results directories for), and do a
   careful overhead study against an unmodified kernel.
