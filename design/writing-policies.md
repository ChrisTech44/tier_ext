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
| `ctx->folio_flags` | `TIER_EXT_F_ANON`, `_ACTIVE`, `_REFERENCED`, `_WORKINGSET`, `_DIRTY`, `_WRITEBACK`, `_MAPPED`, `_SWAPCACHE`, `_UNEVICTABLE`, `_LARGE`; in `folio_added` also `_RETURNED` (a victim handed back) and `_SPLIT` (a split-off tail) |
| `ctx->nr_pages` | folio size in pages (large folios are common in the page cache) |
| `ctx->data_key` | identity of the data, not the frame: file offset or swap slot; for ghost entries |
| `ctx->old_pfn`, `ctx->old_nid` | (`folio_migrated`) where the folio was before |
| `ctx->referenced_ptes` | (`folio_reclaim`) page-table references found just now |
| `ctx->lru_gen`, `ctx->lru_refs` | MGLRU generation and access counter (-1/0 on classic) |
| `ctx->priority` | 12 (gentle) .. 0 (desperate), -1 for events |
| `ctx->reclaim_flags` | `TIER_EXT_RC_KSWAPD`, `_PROACTIVE`, `_CGROUP`, `_NOMINATED` (`folio_reclaim` of a folio this policy nominated) |
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

## Engine policies: choosing victims

An engine keeps its own ordering of folios and nominates victims (see
[engines.md](engines.md) for the concepts and the existing engines). Start
from `policies/fifo.bpf.c`, the smallest complete one:

```c
#include "tier_ext.bpf.h"

#define TE_NR_LISTS 2			/* lists per node: here anon, file */
struct te_extra { };			/* per-node state, under the node's lock */
#include "te_engine.bpf.h"

TIER_EXT_EVENT(folio_added)		/* new folio, or a victim handed back */
{
	struct te_node __arena *n = te_node(ctx->pfn);
	struct te_lruvec *lv = te_lv(ctx->nid);

	if (!n || !lv)
		return;
	te_lock(lv);
	if (te_returned(ctx) || !n->list) {
		n->order = te_order(ctx->nr_pages);
		te_push_head(lv, te_type(ctx), ctx->pfn, n);
	}
	te_unlock(lv);
}

TIER_EXT_EVENT(folio_evicted)  { te_forget(ctx->nid, ctx->pfn); }
TIER_EXT_EVENT(folio_removed)  { te_forget(ctx->nid, ctx->pfn); }
TIER_EXT_EVENT(folio_migrated) { te_migrate(ctx); }

/* the locked loop lives in a global function (see "Verifier" below) */
__noinline int pick(struct tier_ext_victims_ctx *ctx __arg_trusted)
{
	...
	te_lock(lv);
	bpf_for(i, 0, TIER_EXT_MAX_VICTIMS) {
		if (got >= ctx->nr_requested)
			break;
		link = lv->lists[type].tail;		/* oldest */
		n = te_link(link);
		if (!n)
			break;
		pages = te_nominate(ctx, lv, link - 1, n);
		if (!pages)				/* batch full */
			break;
		got += pages;
	}
	te_unlock(lv);
	return got;
}

TIER_EXT_OP(pick_victims, tier_ext_victims_ctx) { pick(ctx); return 0; }
```

Rules for engines:

* **Handle every event that can invalidate a pfn**: `folio_evicted`,
  `folio_removed`, `folio_migrated`. A missed one is harmless (the kernel
  rejects stale pfns and counts them as `victims_invalid`), but wastes a
  nomination.
* **Nominated folios leave your lists** (`te_nominate()` unlinks them) and come
  back through `folio_added` with `TIER_EXT_F_RETURNED` if the kernel keeps
  them. Place them using the flags: `TIER_EXT_F_ACTIVE` means the kernel
  found young page table entries.
* **`folio_added` for a node that is already on a list** (and not RETURNED)
  is a folio put back after someone else isolated it; usually leave it
  where it is.
* **Cover the request.** Whatever you do not nominate is reclaimed by the
  kernel's own ordering, which is stale while your policy does the evicting.
  If your algorithm moved folios around without finding a victim, return
  `TIER_EXT_AGAIN` instead of 0.
* **Respect `ctx->flags`**: do not nominate anon without
  `TIER_EXT_VF_MAY_ANON` (it would come back as skipped). `te_pick_type()`
  splits between anon and file like the kernel does.
* **Page table accesses** only reach you as `folio_referenced` from MGLRU's
  walks. Return `TIER_EXT_AGE` to ask for one (when
  `TIER_EXT_VF_CAN_AGE`).
* **`folio_reclaim` still runs** for your victims (`TIER_EXT_RC_NOMINATED`).
  Return `TIER_EXT_RECLAIM` if your algorithm should not give referenced
  folios a second chance (FIFO, MRU).

### Verifier

Engine code holds a `bpf_spin_lock` and loops, which is where the verifier
gets strict. What worked (D29):

* Under the lock: no helper calls (map lookups, `bpf_ktime_get_ns()`, ...),
  only memory accesses and spin-lock-safe kfuncs (`bpf_tier_ext_add_victim`,
  `bpf_iter_num_*`). Look things up before taking the lock.
* Put each locked loop in a **global function** (`__noinline`, not `static`,
  context argument tagged `__arg_trusted`). It is verified once, whatever the
  caller did before. Two locked loops inlined into one program failed with
  "The sequence of 8193 jumps is too complex".
* Use **`bpf_for(i, a, b)`** for loops, not `for` with a constant bound.
* **Never count in a local variable inside a loop** (`n++`): the verifier
  sees 0, 1, 2, ... as different states and never proves the loop ends. Count
  in a global (`__sync_fetch_and_add(&counter, 1)`) or in map memory.
* Avoid unrolled bit tricks (a 16-step integer square root doubled the paths
  at every step); a bounded `bpf_for` search is fine.
* Arena pointers are declared `struct te_node __arena *`. Globals that hold
  them are declared `__arena_global` so that every program using them is
  tied to the arena.

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
| `lfu` (updated) | + `folio_referenced`, `pick_victims` | counts page table accesses too (anon, mmap'd files) and asks for aging on demand without nominating anything |
| `lru` | engine | the classic LRU in BPF, anon + file |
| `mglru` | engine | MGLRU in BPF: generations, tiers, PID controller, aging via `TIER_EXT_AGE`, ghost refaults |
| `fifo`, `mru`, `s3fifo`, `sampling`, `lhd` | engines | ports of cache_ext's policies (see [engines.md](engines.md)) |
| `garbage` | `pick_victims` | hostile nominations, used to test validation |
