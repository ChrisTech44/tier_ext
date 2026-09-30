# tier\_ext

The goal of tier\_ext is to be able to deploy reclamation logic on a per-lruvec basis.
The motivation behind this is to explore different reclamation logic at different levels
(no global policies---regardless of whether they can be changed or not).

A tier\_ext policy is a BPF `struct_ops` program (`struct tier_ext_ops`) attached
to one memory cgroup on a set of NUMA nodes, i.e. to specific (memcg, node)
lruvecs. Reclaim of those lruvecs asks the policy whether to skip the lruvec,
which anon/file balance to use, which folios to keep or evict, and whether to
demote them, or lets the policy **choose the victims itself**: a policy can
keep its own ordering of the lruvec's folios and nominate page frames, which
the kernel re-validates. Everything the policy leaves at its default, and
every other lruvec, keeps the kernel's classic LRU or MGLRU behaviour.

`policies/` contains complete reclaim engines in BPF, for anon memory and page
cache: the kernel's classic LRU (`lru`) and MGLRU (`mglru`) re-implemented,
and ports of cache_ext's FIFO, MRU, S3-FIFO, LFU (sampling) and LHD policies.
See [design/engines.md](design/engines.md).

| Directory | Contents |
|---|---|
| [`design/`](design/README.md) | Design documents, written for non-experts. Start here. |
| [`kernel-patches/`](kernel-patches/README.md) | The kernel change as patches against 7.3-rc5 |
| `linux/` | Linux 7.3-rc5 with tier\_ext (nested git repo; `git -C linux log`) |
| `policies/` | `tier_ext_load` loader and example policies |
| `tests/` | VM test suite (`tests/run-vm-tests.sh`) |
| `bench/` | Benchmarks in the VM: overhead, cache_ext's file search and YCSB/LevelDB (`bench/README.md`) |

Quick start:

```sh
make -C linux -j$(nproc)      # CONFIG_TIER_EXT=y
make -C policies
sudo policies/build/tier_ext_load -c /sys/fs/cgroup/mygroup -n 0 policies/build/lfu.bpf.o
sudo policies/build/tier_ext_load -c /sys/fs/cgroup/mygroup policies/build/mglru.bpf.o
tests/run-vm-tests.sh         # boots the LinuxDev VM and runs the test suite
```
