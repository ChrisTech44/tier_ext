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

## Results

Final run of the full suite on the committed code (7.3-rc5 + tier_ext, MGLRU on
by default, the `classic` test switching it off at run time): **56 of 56
checks passed**, and the kernel log was clean.

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
