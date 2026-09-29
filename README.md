# tier\_ext

The goal of tier\_ext is to be able to deploy reclamation logic on a per-lruvec basis.
The motivation behind this is to explore different reclamation logic at different levels
(no global policies---regardless of whether they can be changed or not).

A tier\_ext policy is a BPF `struct_ops` program (`struct tier_ext_ops`) attached
to one memory cgroup on a set of NUMA nodes, i.e. to specific (memcg, node)
lruvecs. Reclaim of those lruvecs asks the policy whether to skip the lruvec,
which anon/file balance to use, which folios to keep or evict, and whether to
demote them. Everything the policy leaves at its default, and every other
lruvec, keeps the kernel's classic LRU or MGLRU behaviour.

| Directory | Contents |
|---|---|
| [`design/`](design/README.md) | Design documents, written for non-experts. Start here. |
| `linux/` | Linux 7.3-rc5 with tier\_ext (nested git repo; `git -C linux log`) |
| `policies/` | `tier_ext_load` loader and example policies |
| `tests/` | VM test suite (`tests/run-vm-tests.sh`) |

Quick start:

```sh
make -C linux -j$(nproc)      # CONFIG_TIER_EXT=y
make -C policies
sudo policies/build/tier_ext_load -c /sys/fs/cgroup/mygroup -n 0 policies/build/lfu.bpf.o
tests/run-vm-tests.sh         # boots the LinuxDev VM and runs the test suite
```
