# Testing

## How to run

```sh
make -C linux -j$(nproc)             # kernel with CONFIG_TIER_EXT=y
tests/run-vm-tests.sh                # build policies, boot VM, run all tests
tests/run-vm-tests.sh swappiness pin # or selected tests
SWAP=partition tests/run-vm-tests.sh # swap on a real disk partition instead of zram
```

`run-vm-tests.sh` builds `policies/`, boots `linux/arch/x86/boot/bzImage` in the
LinuxDev VM (snapshot mode, `configs/hmat.conf`), copies the loader, the
policies and `tests/guest/tier_ext_tests.sh` into the guest, runs the suite as
root and powers the VM off. Every check prints `PASS:`/`FAIL:`, and the run
ends with `RESULT: N passed, M failed`.

**Topology** (`configs/hmat.conf`): node 0 has 4 CPUs and 4 GB of "DRAM" (80 ns);
node 1 has 8 GB of CPU-less "far memory" (250 ns), which the kernel places in a
lower memory tier. The guest has no swap of its own, so the suite enables one
(anon memory is only reclaimable with swap): by default a 3 GB zram device
(compressed RAM), or with `SWAP=partition` a swap partition on a blank 4 GB
virtual disk the host creates and attaches for that run only. The suite also
creates test cgroups under `/sys/fs/cgroup`.

## What is tested

Each test checks an effect you can observe from outside the policy (cgroup
`memory.stat`/`memory.numa_stat`, `fincore`, swap usage), not only the
policy's own counters.

| Test | What it shows |
|---|---|
| `attach` | Every callback fires; the policy is never called for another cgroup; debugfs shows it while attached and not after detach |
| `per_node` | Attached to node 1 only, a policy receives zero calls for node 0 even though the cgroup has memory there |
| `attach_rules` | `EBUSY` for a second policy on the same lruvec; OK for the same cgroup on another node and for another cgroup on the same node; `ENOENT` for an unknown cgroup; `EINVAL` for an impossible node or a cgroup without its own memory controller; the root cgroup works |
| `swappiness` | The default kernel reclaims anon once file runs out; with `swappiness` value=0 on that lruvec it reclaims no anon at all |
| `protect` | With node 0's lruvec protected, reclaim takes everything from node 1 and nothing from node 0 |
| `stream` | RECLAIM verdicts are applied at both decision points |
| `tier_demote` | Baseline demotes page cache to node 1; with the policy, clean file is dropped instead, anon is demoted, and nothing is swapped |
| `pin` | A policy answering KEEP/SKIP to everything cannot wedge a cgroup: allocating 400 MB in a 128 MB cgroup completes, `memory.max` holds, and debugfs shows the priority-0 overrides |
| `lfu` | Frequency-based protection keeps a hot file resident while a larger file streams through the same cgroup |
| `classic` | With MGLRU off: the classic scanner calls the policy (never the MGLRU one), and the swappiness, protect and lfu effects hold, including value=200 |
| `stress` | 160 attach/detach cycles of five different policies on two cgroups, while one cgroup thrashes anon (stress-ng) and the other thrashes page cache |
| `mru` | Victim nomination: with `mru`, a 600 MB file read once in a 200 MB cgroup keeps its *first* third in memory (the default keeps the last third); every nomination valid |
| `garbage` | A hostile `pick_victims` (random, low and swept pfns): every nomination accounted for, ≥99% rejected as invalid/foreign, another cgroup's page cache untouched, the cgroup still held at its limit |
| `engines` | The BPF `lru` and `mglru` engines, on the MGLRU *and* the classic kernel engine: an anon workload 1.6× the cgroup completes with anon swapped out, >10,000 victims isolated, invalid nominations <0.1%; `mglru` protects a hot file from a stream (≥90%) and uses page table aging |
| `ports` | cache_ext's `fifo`, `s3fifo`, `sampling`, `lhd`: valid nominations; FIFO keeps the newest data and not the hot file; S3-FIFO keeps the hot file through a scan with no refaults and pgscan < 1.2 × pgsteal |
| `lfu_mmap` | `folio_referenced`: with `lfu` asking for aging on demand, MGLRU's page table walks report an mmap'd hot file's accesses to the policy |
| `events` | `folio_removed` when a cached file is deleted; `folio_migrated` for `migratepages` to node 1; migrated folios then reclaimed as victims on node 1's lruvec with no invalid nominations |
| `update` | `BPF_LINK_UPDATE` replaces `lru` with `mglru` in place: one policy listed throughout, the new one reclaims, nothing left after exit |
| `engine_stress` | 128 attach/detach cycles of `mglru`, `lru`, `mru`, `garbage`, `fifo`, `s3fifo`, `sampling`, `lhd` on two cgroups under anon and page cache pressure |
| kernel log | No WARNING/BUG/Oops/RCU stall/soft lockup/hung task during the whole run (kernel built with `DEBUG_VM`) |

## Results (phase 1: filtering policies)

Final phase 1 run of the full suite (7.3-rc5 + tier_ext, MGLRU on by
default, the `classic` test switching it off at run time): **56 of 56 checks
passed**, and the kernel log was clean. Phase 2 results follow further down.

Measured effects (KiB unless noted; one run each, numbers vary by a few
percent between runs):

**Per-lruvec swappiness**: 300 MB anon + 200 MB file in the cgroup, `memory.reclaim 300M`:

| Engine | Policy | anon reclaimed | file reclaimed |
|---|---|---|---|
| MGLRU | none | 203,812 | 105,072 |
| MGLRU | swappiness=0 | **4** | 208,720 |
| classic | swappiness=0 | **4** | 208,776 |
| classic | swappiness=200 (150M) | 154,656 | **0** |

With swappiness=0, reclaim stops short (`memory.reclaim` returns EAGAIN)
rather than touching anon, which is what the policy asked for.

MGLRU does not age anon just to satisfy a high swappiness while cold file is
available. The kernel's own `memory.reclaim ... swappiness=200` behaves the
same way, so swappiness=200 is only asserted on the classic engine.

**protect** (node 0 lruvec protected, 200 MB cache on each node, reclaim 100M):
node 0 lost 0 KiB, node 1 lost ~102,400 KiB, on both engines.

**tier_demote** (200 MB anon + 200 MB file on node 0, demotion on, reclaim 350M):

| | file demoted to node 1 | anon demoted to node 1 | swapped |
|---|---|---|---|
| no policy | 95,844 | 39,976 | 0 |
| tier_demote (both nodes) | **0** (dropped) | **150,568** | **0** |

**pin** (hostile policy): the 400 MB allocation in a 128 MB cgroup finished
within a second, `memory.peak` stayed under the limit, and the kernel overrode
~168,000 KEEP/SKIP verdicts at priority 0. No OOM kill was needed because
swap was available.

**lfu** (40 MB hot file read 8x, then a 600 MB file streamed once, 200 MB limit):

| Engine | hot file resident, no policy | with lfu |
|---|---|---|
| classic | 0% | 82% (92% in an earlier run) |
| MGLRU | 100% | 100% |

MGLRU's access tiers already protect a file read many times through `read()`.
The classic two-list LRU does not, and there lfu makes the difference.

**Overhead** (`tests/guest/overhead.sh`, 3 rounds, a page-cache-heavy workload
in a 256 MB cgroup that calls `folio_mark_accessed` and reclaim constantly):

| Setup | mean time (s) |
|---|---|
| no policy attached | 0.839 |
| `observe` attached to another cgroup (static key on, pointer NULL) | 0.849 |
| `observe` attached to this cgroup (all 7 callbacks, counting) | 0.856 |

The differences are within the run-to-run spread (±5%). This is a coarse
test; a careful micro-benchmark against an unmodified kernel is future work.

## Swap on a real disk

The full suite also passes (**56 of 56**) with `SWAP=partition`, i.e. swapping
to a partition on a virtual disk instead of zram. The policy guarantees are
unchanged: swappiness=0 reclaims 4 KiB of anon, tier_demote swaps nothing, and
`pin` completes with ~267,000 priority-0 overrides.

One difference is stock kernel behaviour, not tier_ext. Disk swap writes are
asynchronous, and pages under writeback are not counted as reclaimed yet, so
`memory.reclaim` overshoots its target. Without any policy attached,
`memory.reclaim 300M` swapped out the cgroup's entire 310 MB of anon and
dropped its whole page cache (then still reported EAGAIN). With zram, writes
are synchronous and reclaim stops near the target. Because of this, the
classic swappiness=200 check asserts "anon reclaimed more than file" rather
than "file untouched": once the overshoot exhausts anon, reclaim moves on to
file, which "prefer anon" allows.

| swappiness=200, classic, reclaim 150M | anon reclaimed | file reclaimed |
|---|---|---|
| zram | 154,660 | 0 |
| disk partition | 310,308 (all of it) | 151,308 |

## Bugs found by testing

Early runs found problems in the test harness and one policy, not in the
kernel. They are recorded because each taught something about the interface:

* The verifier rejected `lfu` for passing `&ctx->pfn` (a pointer into the
  trusted context) as a map key. Fixed by copying it to the stack, and
  documented in [writing-policies.md](writing-policies.md).
* `tier_demote` attached to node 0 only still let anon be swapped: folios
  demoted to node 1 join node 1's lruvec, which had no policy, and were
  reclaimed from there. That is correct per-lruvec behaviour. The test now
  attaches to both nodes, where `DEMOTE_ONLY` on the last tier means "keep".
* A `folio_accessed` count is 1, not 0, after a single `read()`, so "cold"
  in `lfu` means "used at most once".

## Results (phase 2: victim nomination and engines)

**Test suite.** The full suite (now 120 checks) passes on the final code
with both zram (120/120) and a disk swap partition (120/120), with a clean
kernel log (`DEBUG_VM`). One threshold was lowered during phase 2: `lfu` on
the classic engine keeps between 52% and 100% of the hot file from run to
run (phase 1 recorded 82% and 92%; this phase 52, 68, 100, 100, 100); the
check is now "≥ 50%" instead of "≥ 75%". Without the policy the classic LRU
keeps 0% in every run.

### Engine and port behaviour (from the test logs)

200 MB cgroup; a 40 MB file read 8 times, then a 600 MB file read once
(page cache), then 320 MB of anon written 3 times (zram swap).

| Policy | kernel engine | hot file resident | victims isolated | invalid | notes |
|---|---|---|---|---|---|
| (kernel default) | MGLRU | 100% | – | – | |
| (kernel default) | classic | 0% | – | – | |
| `lru` | MGLRU / classic | 0% / 0% | 473,551 / 424,578 | 0 | same result as the kernel's classic LRU |
| `mglru` | MGLRU / classic | **100% / 100%** | 232,163 / 405,640 | 0 | MGLRU's protection on a classic-LRU kernel |
| `mru` | MGLRU | first third of the scanned file: **82%** (default 0%) | | 0 | impossible with filtering |
| `fifo` | MGLRU | 0%, newest data kept | | 0 | |
| `s3fifo` | MGLRU | **100%**, 0 refaults, pgscan = pgsteal | | 0 | after the fixes below |
| `sampling`, `lhd` | MGLRU | 21% / 5% (4–59% across runs) | | 0 | LFU scores unread read-ahead lowest (41,574 refaults for `sampling`) |

(Numbers from the final zram run.) The anon phase completes with anon
swapped out (~115,000 pages) under every engine. In one earlier run the
kernel's own classic LRU OOM-killed it (exit 137); no engine ever did.

`garbage` (hostile nominations): 20,512 pfns in one run, 20,453 rejected as
invalid (free, tail, kernel pages), 57 as foreign (other cgroups' folios),
2 isolated (te_a's own folios, hit by chance); with the sweep mode 418
foreign and 14 own. Another cgroup's 200 MB of page cache lost 0 KiB, and
te_a stayed at its 200 MB limit.

### Overhead

`bench/run.sh {stock,tier_ext} overhead overhead -r 5`: the unmodified
7.3-rc5 ("stock", same config minus `CONFIG_TIER_EXT`) against tier_ext,
5 rounds each, mean ± standard deviation in seconds, a 2 s settle time after
attaching. Both kernels have `DEBUG_VM`, so absolute numbers are higher
than production ones.

| Workload | stock | tier_ext, none | policy on another cgroup | `observe` | `lru` | `mglru` |
|---|---|---|---|---|---|---|
| `pc_reclaim` (3 × 800 MB read, 256 MB cgroup) | 0.33 ± 0.01 | 0.33 ± 0.02 | 0.32 ± 0.01 | 0.33 ± 0.01 | 0.34 ± 0.01 (+3%) | 0.35 ± 0.01 (+6%) |
| `pc_hit` (20 × 200 MB cached, 4 KiB reads) | 0.81 ± 0.00 | 0.82 ± 0.01 | 0.82 ± 0.01 | 0.82 ± 0.00 | 0.82 ± 0.00 | 0.82 ± 0.01 |
| `anon_fault` (1 GiB fresh anon) | 0.07 ± 0.01 | 0.06 ± 0.01 | 0.06 ± 0.01 | 0.06 ± 0.02 | 0.06 ± 0.01 | 0.06 ± 0.02 |
| `anon_reclaim` (3 × 512 MB in 256 MB, zram) | 1.83 ± 0.02 | 1.84 ± 0.03 | 1.84 ± 0.04 | 1.96 ± 0.01 (+7%) | 2.05 ± 0.02 (+11%) | 2.10 ± 0.02 (+14%) |

Without a policy, and with a policy on *another* cgroup, tier_ext costs
nothing measurable. With a policy on the cgroup, the cost appears where
events are dense: anon reclaim, where every folio is added, nominated,
isolated, swapped out and reported evicted. Even `observe`, which only
counts, adds 7% there; the engines add their list maintenance under a
per-node spin lock on top.

An earlier run showed `mglru` at +74% on `pc_reclaim` and `anon_fault` at
+60% with a policy attached elsewhere. The first was a real bug in the BPF
MGLRU (fixed, see below); the second was a measurement artifact of timing
the workload right after loading a policy, which the settle time removed
(a direct check showed 49–75 ms for every setup).

### What phase 2 testing found

* **Stale entries from folios freed while isolated** (1 invalid nomination
  per ~400,000): fixed with a second `folio_removed` site
  (`move_folios_to_lru()`), D24.
* **MGLRU's walk batch not flushed** after reclaiming victims:
  `clear_mm_walk()` warned. Fixed by flushing like `evict_folios()`.
* **The stale fallback** (D22): S3-FIFO's first port under-nominated and the
  kernel's fallback evicted read-ahead before it was read (48,000–86,000
  file refaults on one sequential pass; 0 after `TIER_EXT_AGAIN`, D25).
* **Large folios and frequency**: S3-FIFO promoted scans to its main queue
  because one read pass can mark a 2 MB folio accessed twice; dedup within
  1 ms fixed it (engines.md).
* **Rejected victims recycled**: the BPF MGLRU put victims the kernel could
  not reclaim (locked read-ahead) back into the oldest generation, where they
  were nominated again: 51% of nominations came back, page cache reclaim took
  74% longer than the kernel's MGLRU. Applying `evict_folios()`'s rule
  ("don't add rejected folios to the oldest generation") brought it to +6%.
* **Verifier structure** for engines (D29).
* Harness bugs: `cp` charges the copy to the writer's cgroup; a test
  threshold counted pages where the kernel reports folios (THPs); the
  benchmark runner split a multi-word argument (the first YCSB run was
  discarded).

### File search (cache_ext's Figures 6/9)

`bench/run.sh tier_ext filesearch filesearch -e both -i 3`: ripgrep searches
the Linux 7.3-rc5 source tree (1.8 GB, 96k files) 10 times in a cgroup
limited to 1 GiB, swap off, 3 runs per configuration. Seconds for the 10
passes, mean ± sd (lower is better):

| Policy | MGLRU kernel | classic kernel |
|---|---|---|
| kernel default | 10.93 ± 0.62 | 12.05 ± 0.22 |
| **`mru`** | **8.48 ± 0.55 (−22%)** | **8.65 ± 0.05 (−28%)** |
| `mglru` (BPF) | 12.37 ± 0.43 | 11.29 ± 0.23 |
| `lru` (BPF) | 13.37 ± 0.54 | 12.26 ± 0.56 |
| `s3fifo` | 13.46 ± 0.50 | 12.74 ± 0.32 |
| `fifo` | 13.68 ± 0.50 | 13.16 ± 0.16 |
| `lhd` | 17.22 ± 0.27 | 16.01 ± 0.09 |
| `sampling` | 18.09 ± 0.63 | 16.72 ± 0.30 |

A repeated scan over data larger than memory is the case where recency is
exactly wrong: every LRU variant evicts the data just before it is needed
again. MRU keeps a fixed part of the tree resident across passes, which is
cache_ext's result (the paper reports up to 2× on its machine; here the
data-to-memory ratio is smaller and I/O is fast, so the gain is 22–28%).
It needed victim nomination: with candidate filtering, a policy cannot
evict the newest folios. The BPF `mglru` is within 13% of the kernel's on
MGLRU and 6% *faster* on the classic kernel; the other engines pay for
per-folio BPF work on a workload of ~96,000 small files, and the sampling
policies (LHD, LFU) additionally score unread read-ahead lowest. (Refault
counts are the kernel's shadow-entry statistics, which are not comparable
across engine policies: the kernel's generations do not advance under them.)
