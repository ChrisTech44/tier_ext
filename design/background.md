# Background: how Linux reclaims memory

This page explains just enough of the Linux memory manager to follow the rest
of the design. Terms in **bold** are used throughout the other documents.

## Pages, folios, and why memory must be reclaimed

Memory is managed in 4 KiB **pages**. The kernel groups physically contiguous
pages that belong together (say, 16 KiB of one file) into a **folio**; a folio
is the unit reclaim works with.

Most memory in use holds one of two things:

* **File pages** (the *page cache*): copies of file contents. If the page is
  unchanged ("clean") it can be dropped at any time and re-read from disk
  later. If it was modified ("dirty") it must be written back first.
* **Anonymous (anon) pages**: process memory with no file behind it (heap,
  stack). The only way to free it is to write it to **swap** (a disk area, or
  compressed RAM such as zram) and read it back on the next access.

Linux uses free memory as cache, so memory is almost always nearly full. When
an allocation needs memory, or a cgroup exceeds its limit, the kernel must pick
some folios and free them. That is **reclaim**. It runs in the background
(**kswapd**), synchronously in the allocating task (**direct reclaim**), when a
cgroup hits `memory.max`/`memory.high`, or on request (**proactive reclaim**,
e.g. writing to `memory.reclaim`).

The central question of reclaim is **which folios to free**: the ones least
likely to be needed soon. Guessing wrong causes *refaults*: the data is read
back from disk or swap moments later.

## LRU lists: the classic policy

The kernel approximates "least likely to be needed" with *least recently used*.
Folios sit on **LRU lists**, four per group of memory:

* inactive anon, active anon, inactive file, active file (plus unevictable).

New folios start on an inactive list. Reclaim scans the cold end (tail) of the
inactive lists. For each folio it checks whether it was accessed recently (the
"referenced" bits in the page tables and in the folio). Referenced folios get a
second chance or are promoted to the active list; unreferenced ones are
reclaimed. Active lists are slowly drained back to inactive.

The balance between reclaiming anon and file is steered by **swappiness**
(0..200): 0 means "avoid swapping anon", 200 means "prefer anon".

## MGLRU: the multi-generational LRU

Newer kernels can instead use **MGLRU**. Instead of two lists per type it keeps
up to four **generations** (oldest to youngest). *Aging* periodically creates a
new youngest generation and moves recently accessed folios into it (found by
walking page tables). *Eviction* takes folios from the oldest generation. Within
a generation, folios accessed several times through system calls sit in higher
**tiers** and are protected from eviction.

Either engine can be active; `/sys/kernel/mm/lru_gen/enabled` switches between
them at run time. tier_ext supports both.

## Memory cgroups (memcgs)

A **cgroup** is a group of processes (a container, a service). The memory
controller (**memcg**) accounts every folio to exactly one cgroup and enforces
limits such as `memory.max`. Each cgroup has an id (on cgroup v2, the inode
number of its directory in `/sys/fs/cgroup`).

## NUMA nodes and memory tiers

On a machine with several memory controllers, memory is split into **NUMA
nodes**. Memory attached to a far socket, or a CXL memory expander, is slower
than local DRAM. The kernel groups nodes of similar speed into **memory tiers**.
Some nodes, like a CXL device, have memory but no CPUs.

When reclaiming a fast node, the kernel can **demote** a folio: move it to a
node in a slower tier instead of dropping or swapping it. The data stays in
memory, just slower. (Enabled via `/sys/kernel/mm/numa/demotion_enabled`.)
Folios on the slowest tier can only be swapped or dropped.

## The lruvec: the unit tier_ext works on

Putting the last three sections together: every folio belongs to one cgroup and
lives on one node, and the kernel keeps a separate set of LRU lists (or MGLRU
generations) **for every (memcg, node) pair**. That set is called an
**lruvec** (`struct lruvec`). Reclaim always works on one lruvec at a time:

```
          node 0 (DRAM)        node 1 (CXL, slower)
memcg A   lruvec(A,0)          lruvec(A,1)
memcg B   lruvec(B,0)          lruvec(B,1)
root      lruvec(root,0)       lruvec(root,1)
```

Before tier_ext, every lruvec in the system follows the same global policy.
tier_ext lets each box in that table have its own.

## BPF and struct_ops

**BPF** lets user space load small programs into the kernel. A verifier checks
every program before it runs: it must terminate, may only access memory it is
allowed to, and may only call approved helper functions. So a BPF program
cannot crash the kernel the way a kernel module can.

**struct_ops** is the BPF mechanism for implementing a kernel "ops table" (a
struct of function pointers) with BPF programs. The TCP congestion control and
the `sched_ext` scheduler framework use it. tier_ext defines a new ops table,
`struct tier_ext_ops`.
