# Benchmarks

Measurements of tier_ext in the LinuxDev VM (`configs/hmat.conf`: node 0 = 4
CPUs + 4 GB, node 1 = 8 GB CPU-less). Results and methodology are in
[../design/testing.md](../design/testing.md).

## One-time setup

```sh
make -C linux -j$(nproc)                     # the tier_ext kernel
git -C linux worktree add ../baseline 3f7b39dd7   # unmodified 7.3-rc5
cp linux/.config baseline/ && (cd baseline && scripts/config --disable TIER_EXT &&
    make olddefconfig && make -j$(nproc))    # "stock": same config minus TIER_EXT
bench/setup.sh                               # the data disk, ~10 minutes, needs network in the guest
```

`bench/setup.sh` creates `bench/cache/data.qcow2` (not in git) and fills it,
inside the guest: the `.deb` packages of ripgrep, fio, cmake, yaml-cpp,
snappy and libbpf; cache_ext's LevelDB and My-YCSB (from `~/silence/cache_ext`,
override with `CACHE_EXT=`) built there; a 20M-record YCSB database (16 B
keys, 200 B values, 4.1 GB); and the Linux 7.3-rc5 source tree (1.8 GB,
96k files). The VM's system disk runs in snapshot mode, the data disk does
not, so this happens once.

## Running

```sh
bench/run.sh KERNEL NAME BENCH [ARGS...]    # appends CSV to bench/results/NAME.csv
bench/summarize.py bench/results/*.csv      # mean ± sd tables (Markdown)
```

| BENCH | What | Typical ARGS |
|---|---|---|
| `overhead` | stock vs tier_ext without a policy, with `observe` on another cgroup, on this cgroup, and with the `lru`/`mglru` engines; page cache with reclaim, page cache hits, anon faults, anon with reclaim | `-r 5` |
| `filesearch` | cache_ext's file search: `rg write` over the source tree 10 times in a 1 GiB cgroup, swap off | `-e both -i 3 [POLICY...]` |
| `ycsb` | cache_ext's YCSB on LevelDB, scaled down: 1 GiB cgroup, 4 threads, zipfian 0.99, 20 s warm-up + 60 s | `-e y -i 2 -w "c b a f" [POLICY...]` |

`KERNEL` is `tier_ext` (`linux/`) or `stock` (`baseline/`). On the stock
kernel only the "no policy" setups run.

`bench/vm.sh start|stop|ssh` manages the VM with the data disk attached, for
interactive use.

## Scale compared to cache_ext

cache_ext ran on a 64-core bare-metal machine with NVMe, databases of tens of
GiB and cgroups of several GiB; this VM has 4 vCPUs, a 32 GB virtual disk
backed by the host's page cache and NVMe, and 12 GB of RAM. The workloads
keep cache_ext's shape (same programs, same access patterns, same
data-to-memory ratio of roughly 2-4x) at a smaller size. Absolute numbers are
not comparable with the paper; the comparisons between policies are.
