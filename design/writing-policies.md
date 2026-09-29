# Writing a policy

## Build requirements

* A kernel built with `CONFIG_TIER_EXT=y` (the `linux/` tree here; the
  LinuxDev config plus `scripts/config --enable TIER_EXT`).
* clang with the BPF target, `bpftool`, `libelf-dev`, `zlib1g-dev`.

`make -C policies` generates `vmlinux.h` from `linux/vmlinux`, builds the
in-tree libbpf statically, and produces `policies/build/tier_ext_load` and one
`build/<name>.bpf.o` per `<name>.bpf.c`. The loader is statically linked
against libbpf, so it can be copied into the VM as is.

## A minimal policy

```c
// policies/file_first.bpf.c
#include "tier_ext.bpf.h"

char _license[] SEC("license") = "GPL";

/* Never reclaim a referenced anon folio; let the kernel decide the rest. */
TIER_EXT_OP(folio_reclaim, tier_ext_folio_ctx)
{
	if (te_anon(ctx) && ctx->referenced_ptes > 0)
		return TIER_EXT_KEEP;
	return TIER_EXT_DEFAULT;
}

SEC(".struct_ops.link")
struct tier_ext_ops file_first = {
	.folio_reclaim	= (void *)folio_reclaim,
	.name		= "file_first",
};
```

* `TIER_EXT_OP(op, ctx_type)` declares a decision callback whose body sees its
  argument as `ctx`. `TIER_EXT_EVENT(op)` declares an event callback.
* Return `TIER_EXT_DEFAULT` whenever the policy has no opinion.
* Leave out callbacks you don't need.
* Leave `cgroup_id` and `node_mask` unset; the loader fills them in.

## Attaching

```sh
# policy for cgroup "db" on node 0 only, until Ctrl-C
sudo ./tier_ext_load -c /sys/fs/cgroup/db -n 0 build/file_first.bpf.o

# run a command with the policy attached, then detach and print counters
sudo ./tier_ext_load -c /sys/fs/cgroup/db build/lfu.bpf.o -- ./benchmark

# set a policy's global variable before loading
sudo ./tier_ext_load -c /sys/fs/cgroup/db -s value=0 build/swappiness.bpf.o
```

On exit the loader prints the policy's global variables, and the non-zero
entries of `u64` arrays (e.g. `scan[1] = 4096`). The easiest way to export
statistics from a policy is to use plain global variables.

While attached, check `/sys/kernel/debug/tier_ext` for the kernel's counters.

Errors when attaching:

| errno | meaning |
|---|---|
| `EBUSY` | one of the target lruvecs already has a policy |
| `ENOENT` | no cgroup with that id |
| `EINVAL` | the cgroup does not have the memory controller enabled itself, a node in the mask cannot exist, or `flags` is not 0 |

## Useful context fields

| Field | Meaning |
|---|---|
| `ctx->pfn` | stable id of the folio while it is resident; the usual map key |
| `ctx->nid`, `ctx->cgroup_id` | which lruvec |
| `ctx->folio_flags` | `TIER_EXT_F_ANON`, `_ACTIVE`, `_REFERENCED`, `_WORKINGSET`, `_DIRTY`, `_WRITEBACK`, `_MAPPED`, `_SWAPCACHE`, `_UNEVICTABLE`, `_LARGE` |
| `ctx->referenced_ptes` | (`folio_reclaim`) page-table references found just now |
| `ctx->lru_gen`, `ctx->lru_refs` | MGLRU generation and access counter (-1/0 on classic) |
| `ctx->priority` | 12 (gentle) .. 0 (desperate), -1 for events |
| `ctx->reclaim_flags` | `TIER_EXT_RC_KSWAPD`, `_PROACTIVE`, `_CGROUP` |
| `ctx->engine` | `TIER_EXT_ENGINE_CLASSIC` or `_MGLRU` |
| lruvec ctx `nr_anon`, `nr_file` | pages on this lruvec's LRU |
| lruvec ctx `swappiness` | what the kernel would use |

## Rules of thumb

* Callbacks run in atomic context, some with interrupts off. Keep them short:
  a map lookup or two.
* Key per-folio state by `pfn` in a `BPF_MAP_TYPE_LRU_HASH`. The pfn is reused
  after a folio is freed; delete the entry in `folio_evicted` and let the LRU
  hash age out entries for folios freed in other ways.
* Copy context fields to the stack before passing them to map helpers
  (`u64 pfn = ctx->pfn; bpf_map_lookup_elem(&m, &pfn)`). The verifier rejects
  pointers into the context as map keys.
* `KEEP`/`SKIP`/`DEMOTE_ONLY` are hints that are dropped at priority 0. Watch
  the `overridden` counter; if it grows quickly, the policy is fighting real
  memory pressure.

## The example policies

| Policy | Callbacks | What it demonstrates |
|---|---|---|
| `observe` | all | counts every call per node, changes nothing |
| `swappiness` | `swappiness` | per-(cgroup, node) swappiness, which cgroup v2 lacks |
| `protect` | `lruvec_shrink` | per-node `memory.low`: skip small lruvecs |
| `stream` | `folio_scan`, `folio_reclaim` | recycle page cache aggressively for scan-heavy jobs |
| `lfu` | events + `folio_scan` | frequency-based protection using per-folio state in a map |
| `tier_demote` | `folio_reclaim` | drop clean file instead of demoting; demote anon, never swap |
| `pin` | `lruvec_shrink`, `folio_scan`, `folio_reclaim` | hostile policy used to test the safety net |
