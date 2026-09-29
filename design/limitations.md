# Limitations and future work

## Current limitations

* **The kernel's ordering goes stale under an engine policy** (D22, D28).
  While a policy chooses the victims, MGLRU's generations stop advancing and
  the classic LRU's lists stop aging. What the policy leaves to the engine
  (its fallback) is then reclaimed in a poor order. Engines should cover
  their whole budget (`TIER_EXT_AGAIN` helps); fallback is a safety net.
* **Engines only know folios added after they attach.** Folios already on
  the LRU when a policy attaches are invisible to it and are reclaimed by the
  engine's fallback. Attach before the workload starts, or let the fallback
  drain the old folios. A kernel "replay the LRU at attach" would fix this.
* **Page table accesses are only visible on MGLRU** (D26). On the classic
  engine, an engine policy learns about young PTEs only when the kernel
  reclaims (and then activates) a nominated folio. `TIER_EXT_AGE` is ignored
  on the classic engine.
* **Frequency on large folios is coarse.** One sequential pass can mark a
  large page cache folio accessed more than once (see `s3fifo` in
  [engines.md](engines.md)). The access event carries no offset or time, so
  policies must deduplicate on their own.
* **Split tails and putbacks after other isolations** are reported as
  `folio_added`, but a policy does not learn *where* the head of a split folio
  was; tails start as new folios. Folios moved by `mlock`/`munlock` between
  the evictable and unevictable lists produce no event.
* **Writeback completion is not an event.** The kernel rotates a folio whose
  writeback finished to the cold end of its own list
  (`folio_rotate_reclaimable()`); an engine that got it back as RETURNED
  (dirty) does not learn that it became clean.
* **No inheritance** (D5). A policy covers exactly one memcg. Folios charged to
  child cgroups are on the children's lruvecs.
* **Aging in the kernel's own engines is not programmable.** Engine policies
  implement their own aging instead.
* **No per-lruvec scan pressure.** A policy can skip an lruvec or change its
  anon/file balance, and an engine decides which folios go, but not how many
  pages a pass scans in it.
* **Demotion target is fixed.** Policies can forbid or require demotion but not
  pick the node.
* **Slab shrinkers are untouched.** tier_ext affects LRU folios only; the
  per-memcg slab shrinking that follows each lruvec is unchanged.
* **Node mask limited to 64 nodes** (`u64 node_mask`); engine policies keep
  per-node state for 64 nodes (`TE_MAX_NODES`).
* **Engine memory**: 16 bytes of arena per page frame of the machine (0.4%),
  per loaded engine policy, whether or not its cgroup uses that memory.
* **DEMOTE_ONLY and large folios.** On a kernel without THP migration, large
  folios cannot be demoted, so DEMOTE_ONLY keeps them until priority 0.

## Future work

1. **Populate at attach**: walk the lruvec's lists once and report every
   folio (oldest first) through `folio_added`, so an engine can take over a
   running workload. Not done yet because the walk must drop the LRU lock
   periodically, and a cursor (marker entry) left in a kernel LRU list
   would be visible to MGLRU's and the classic scanner's `lru_to_folio()`;
   a safe design needs isolation in batches or a per-list generation count.
2. **Access details in events**: offset within a large folio, or a coarse
   timestamp, so frequency policies can tell repeated use from one scan.
3. **Optional inheritance.** A `TIER_EXT_F_DESCENDANTS` flag, with the pointer
   propagated at cgroup creation and at attach time.
4. **Scan scaling.** Let `lruvec_shrink` return a scaling factor for
   `nr_to_scan`, not just skip/default.
5. **kfuncs for expensive information** (e.g. refault distance from the
   workingset shadow entries, or per-lruvec refault rates), computed on demand
   instead of in every context.
6. **Demotion target choice.** Let `folio_reclaim` return a preferred node among
   the allowed demotion targets.
7. **State hand-over on update**: `.update` (D30) swaps programs without a
   gap, but the new policy starts empty.
8. **A writeback-completion event**, so engines can re-order folios that
   became reclaimable.
