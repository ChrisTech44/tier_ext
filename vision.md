# tier\_ext Vision

Current Linux code and proposed other systms (tier\_bpf, reclaim\_ext, tierbpf, etc) all operate under a global reclaim policy that is either static
or can be modified using BPF. I want to use BPF to deploy custom reclaim policies at a finer granularity, per-lruvec. I want each (node, memcg) to be
able to have custom logic and fall back to the default (LRU, MGLRU) when it does not apply.

You are responsible for planning, designing the mechanisms, and implementing tier\_ext. You are in a git repository that is private. THis is also the Linux kernel inside this repo. You are also free
to test what you have designed using my virtual machine environment in ~/silence/LinuxDev/. You must ensure the changes you make are functional. You are not allowed to read outside files except those in ~/silence and here. Make commits often so progress can be recorded. You are only allowed to modify kernel code in this directory and its subdirectories. Record all design design decisions in a /design folder so that any non-expert can easily understand what is going on.

---

# Handoff for the next agent

Everything below was written after the first implementation phase. It
describes what exists, how to work in this repository, what was learned, and
the next steps in enough detail to continue without re-deriving anything. The
rules above still apply. Read `design/` for the full rationale; this document
is the map.

## 1. Current state (phase 2 done, 2026-09-30)

Phases 1-4 of the previous handoff are implemented, tested and documented
(`design/`, decisions D19-D30, `design/engines.md`).

**Kernel** (`git -C linux log 3f7b39dd7..`, 7 patches, 10 files, ~2,100 lines):

| Feature | Where |
|---|---|
| `pick_victims` + kfunc `bpf_tier_ext_add_victim()`: policies nominate pfns, the kernel re-validates and reclaims them, survivors come back via `folio_added` + `TIER_EXT_F_RETURNED` | `mm/tier_ext.c` (`__tier_ext_pick_victims`, `tier_ext_isolate_victim`), `mm/vmscan.c` (`tier_ext_reclaim_victims`, `tier_ext_lru_gen_victims`, `tier_ext_classic_victims`) |
| return flags `TIER_EXT_AGE` (MGLRU page table aging on demand) and `TIER_EXT_AGAIN` (call again, ≤16 empty batches) | same |
| events `folio_removed`, `folio_migrated`, `folio_referenced`, split tails (`TIER_EXT_F_SPLIT`) | `mm/folio.c`, `mm/vmscan.c` (`move_folios_to_lru`, `walk_update_folio`), `mm/migrate.c`, `mm/huge_memory.c` |
| `data_key` (file offset / swap slot identity) in every folio ctx, taken before `__remove_mapping()` for evictions | `tier_ext_data_key()` |
| struct_ops `.update` (in-place policy replacement), callbacks fetched once per call (`TE_OP`) | `mm/tier_ext.c` |

**Policies** (`policies/`): engine library `te_engine.bpf.h` (per-pfn 16-byte
nodes in a BPF arena, per-node spin-locked lists; loader runs `te_init` and
`te_periodic`); engines `lru` (classic LRU in BPF), `mglru` (MGLRU in BPF:
gens, tiers, PID, ghost refaults, AGE), cache_ext ports `fifo`, `mru`,
`s3fifo`, `sampling`, `lhd`; hostile `garbage`; `lfu` now counts page table
accesses and requests aging on demand. Loader: `-u NEXT.bpf.o` + SIGUSR1
replaces a policy in place.

**Tests**: 120 checks; final run 120/120 with a disk swap partition, 119/120
with zram (lfu-on-classic residency 68% vs the old 75% threshold, since
lowered to 50%; it varies 52-100% between runs). Clean log (`DEBUG_VM`). New tests: mru, garbage, engines, ports, lfu_mmap, events,
update, engine_stress.

**Benchmarks** (`bench/`, full tables in `design/testing.md`, raw CSVs in
`bench/results/`; stock = unmodified 7.3-rc5 from the `baseline/` worktree,
same config minus TIER_EXT; all in the 4-vCPU VM with `DEBUG_VM`):

| What | Result |
|---|---|
| overhead, no policy / policy on another cgroup | not measurable vs stock on all 4 workloads |
| page cache reclaim, `lru` / `mglru` | +3% / +6% |
| anon reclaim (worst case), `observe` / `lru` / `mglru` | +7% / +11% / +14% |
| page cache hits, anon faults, any setup | no measurable cost |
| file search (rg over 1.8 GB tree, 1 GiB cgroup) | `mru` −22% time on MGLRU, −28% on classic (cache_ext's result); BPF `mglru` within 13% of the kernel's; LHD/LFU-sampling ~50% slower |
| YCSB on LevelDB (20M records, 1 GiB) | all engines within ~±5% of kernel MGLRU (= run spread); `lru` +4% on A/F; LHD/sampling −7..−17%; no LHD/S3-FIFO win at this scale (unlike the paper) |

Data disk `bench/cache/data.qcow2` (not in git) holds tools, LevelDB/My-YCSB
and datasets; `bench/setup.sh` rebuilds it.

**Stability evidence (and its limits)**: the final ~2 h chain (both test
suites, overhead, file search, YCSB: six VM boots, the longest ~1.5 h of
continuous LevelDB load) produced no WARNING/BUG/Oops/RCU stall/soft lockup/
hung task. Only `DEBUG_VM` was on: **no lockdep (`PROVE_LOCKING`), no KASAN**,
no single multi-hour boot, and only 4 vCPUs. One real WARNING was hit during
development (unflushed MGLRU walk batch) and fixed (patch 0004).

**GitHub**: the outer repo is pushed (`origin/main`, with the user's approval,
2026-09-30). The nested `linux/` repo still has no remote; the kernel reaches
GitHub only as `kernel-patches/0001-0007` (verified to `git am` onto
3f7b39dd7 and reproduce `linux/` HEAD). `vision.md`, `baseline/` and
`bench/cache/` are gitignored. Push again only with the user's approval.

## 2. Repository structure

```
design/        docs; engines.md is new; decisions D1-D30
kernel-patches/ 0001-0007 against 7.3-rc5 (regenerate after kernel commits)
linux/         nested repo, main branch; baseline commit 3f7b39dd7
baseline/      git worktree of linux/ at 3f7b39dd7, built as the "stock" kernel (gitignored)
policies/      te_engine.bpf.h, engines, filters, loader
tests/         run-vm-tests.sh + guest suite (+ te_resident.c)
bench/         vm.sh, setup.sh, run.sh, summarize.py, guest/*.sh, results/*.csv, cache/ (gitignored)
```

## 3. Workflow

As before (build `linux/`, `make -C policies`, `tests/run-vm-tests.sh`,
`SWAP=partition`). Benchmarks: `bench/run.sh {stock|tier_ext} NAME BENCH ARGS`,
then `bench/summarize.py bench/results/*.csv`. Commit kernel changes in
`linux/`, regenerate `kernel-patches/`, bump the gitlink. The Bash tool's
background jobs die after 2 h: launch long chains with `setsid nohup` and
poll their logs.

## 4. Lessons learned (in addition to the old ones)

* **Verifier and engines**: locked loops belong in global functions
  (`__noinline`, `__arg_trusted` ctx); use `bpf_for`; never keep a
  loop-carried counter in a local (count in memory); no unrolled bit tricks.
  "The sequence of 8193 jumps is too complex" = one of these.
* Variable-offset writes through BTF pointers are rejected: output goes
  through kfuncs.
* **Fallback is stale**: whatever an engine leaves to the kernel is reclaimed
  by the kernel's frozen ordering (evicts read-ahead → refaults). Engines must
  cover their budget; use `TIER_EXT_AGAIN`.
* **Returned victims must not go straight back to the eviction end** (the
  kernel's evict_folios rule); otherwise locked read-ahead cycles.
* **Large folios** make "access frequency" coarse: one sequential pass can
  mark a 2 MiB folio accessed twice (s3fifo dedups within 1 ms).
* Test thresholds must count folios, not pages (THPs, large page cache folios).
* `cp` charges the file to the writer's cgroup; drop caches before reading it
  into a test cgroup.
* `pkill -f pattern` from a tool shell can kill that shell.

Earlier lessons (phase 1), still valid:

* **Map keys:** the verifier rejects `&ctx->field` as a map key or value
  pointer. Copy it to the stack first (`u64 pfn = ctx->pfn;`).
* **`BPF_PROG()` reserves the name `ctx`.** That is why `tier_ext.bpf.h` wraps
  bodies in an inner function.
* **No recursion guard for struct_ops.** Callbacks can nest, e.g.
  `folio_accessed` from an interrupt during `folio_scan`. Policies must be
  reentrant (engine policies use irq-disabling `bpf_spin_lock`).
* **MGLRU ignores high swappiness while file is available.** "swappiness=200
  → anon" is only testable on classic.
* **Disk swap overshoot.** With async disk swap, `memory.reclaim` overshoots
  its target and returns EAGAIN. Stock behaviour.
* **Per-node semantics are real.** Folios demoted from node 0 join node 1's
  lruvec and are governed by node 1's policy, or none.
* **Test harness traps:** Python `mmap.mmap(-1, n)` is *shared* (shmem); use
  `MAP_PRIVATE|MAP_ANONYMOUS`. `/sys/devices/system/node/nodeN/reclaim`
  rejects `swappiness=`. `$(func)` runs in a subshell. `pgrep -f pattern`
  inside `ssh '...'` matches its own shell. New QEMU needs `serial=` on
  `-device`.
* The kernel tree's top-level `.gitignore` was restored in the first tier_ext
  commit; that hunk is not part of the feature.

## 5. Future direction

1. **Populate at attach** (report existing folios to a new engine). Needs a
   safe resumable walk of kernel LRU lists (see limitations.md).
2. **Engine efficiency**: per-node locks are global per lruvec; shard, or
   batch events per CPU. Measure lock contention with many CPUs.
3. **Access detail in events** (offset/time) for frequency policies on large
   folios; a writeback-completion event.
4. Remaining small items: inheritance flag, scan-pressure scaling, demotion
   target choice, state hand-over on `.update`.
5. Bigger benchmarks on real hardware (cache_ext's full-size YCSB, twitter
   traces), and multi-tenant runs (different engines per cgroup/node at once).

## 6. Implementation steps

Phases 1-4 of the previous plan are done. For the next agent, a suggested
order:

0. **Harden first**: build `linux/` with `PROVE_LOCKING`, `KASAN` (and
   `DEBUG_ATOMIC_SLEEP`), run the full suite and a long single-boot soak
   (e.g. YCSB + file search + `engine_stress` looping for 8+ hours, engines
   attached and swapped with `-u`). The engine policies take a BPF spin lock
   inside callbacks that run under the LRU lock; lockdep has never seen that.
1. Populate at attach.
2. Engine lock sharding + a contention benchmark with more vCPUs.
3. Per-tenant experiment: two cgroups with different engines on the same
   node, and one cgroup with different engines on the DRAM vs CXL node.

## 7. Guardrails

* Fallback to default behaviour must stay the norm: every new callback is
  optional, and "no answer" = kernel behaviour.
* Never let a policy make memory unreclaimable. Extend the priority-0
  override to anything new that restricts reclaim.
* Hooks stay behind the `tier_ext_attached` static key; zero cost when unused.
* Keep kernel state read-only to BPF except the explicitly whitelisted output
  fields.
* Report results honestly in `design/testing.md`, including failures and
  threshold changes, and never claim a result that wasn't observed in a run.
