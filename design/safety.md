# Safety

A reclaim policy runs in the most sensitive part of the kernel: code that runs
when the machine is out of memory, often with locks held and interrupts off.
tier_ext is designed so that no policy can make things worse than "a bad
policy". Here is how each class of problem is ruled out.

## 1. Memory safety: the BPF verifier

Policies are BPF programs, so the verifier guarantees before load that they
terminate, only touch memory they may, and only call approved helpers.

On top of that, tier_ext makes the kernel's data **read-only** to policies
(`btf_struct_access` rejects every write). A policy can read `struct folio`
fields but cannot change flags, lists or counters. Its only ways to influence
reclaim are its **return values** and the **pfns it nominates**, and the
kernel validates both (section 6).

## 2. Atomic context: callbacks cannot sleep

`folio_scan` and `folio_added` run with the lruvec's LRU spinlock held and
interrupts disabled. The other callbacks run inside an RCU read-side section.
`check_member` rejects sleepable programs for every callback, so a policy
cannot block while holding these.

## 3. Liveness: priority 0 overrides the policy

Reclaim retries with increasing urgency: `sc->priority` goes from 12 (scan
1/4096 of each list) down to 0 (scan everything). tier_ext honours the
restrictive answers (`KEEP` from `folio_scan`/`folio_reclaim`, `SKIP` from
`lruvec_shrink`, `DEMOTE_ONLY`) **only while priority > 0**. At priority 0 they
are treated as `DEFAULT` and counted as `overridden`.

So the worst a policy can do is make reclaim of its lruvecs slower (more
passes). It cannot make memory unreclaimable, cause an OOM kill the kernel
would not otherwise do, or make an allocation hang. The `pin` test policy
answers KEEP and SKIP for everything. Under it, a cgroup allocating 3x its
`memory.max` still completes, the limit holds, and debugfs shows the overrides
(see [testing.md](testing.md)).

The permissive answers (`RECLAIM`, `NO_DEMOTE`, a swappiness value) are always
honoured. They can make a policy's own lruvecs thrash, but that only affects
the cgroup the policy was attached to, which is the point of per-lruvec
policies.

## 4. Kernel invariants the policy cannot override

Decisions the kernel makes for correctness never reach the policy, or are
applied regardless of its answer:

* unevictable / mlocked folios, folios in `VM_LOCKED` VMAs;
* folios in zones this reclaim is not allowed to use;
* folios that are being freed, or isolated by another thread;
* contended rmap locks (the folio is simply retried later);
* "no swap space" and `may_swap == 0` (swappiness is forced to 0 afterwards);
* anon-only proactive reclaim (`memory.reclaim` with `swappiness=max`);
* explicit user reclaim of specific folios (`MADV_PAGEOUT`, DAMON), which
  ignores references, and therefore the policy too.

## 5. Lifetime and concurrency

* **Callbacks vs detach.** Every call happens inside `rcu_read_lock()`, and
  detach clears the lruvec pointers and then calls `synchronize_rcu()` before
  freeing anything. By the time detach returns no callback is running, and BPF
  may free the programs.
* **Attach vs attach.** A mutex serializes attach/detach; the "one policy per
  lruvec" check and the pointer publication happen under it.
* **Cgroup removal.** The policy holds a reference on its memcg, so the lruvecs
  it points into stay allocated even if the cgroup directory is removed while
  the policy is attached.
* **Memory hotplug.** Every possible node has a per-memcg lruvec from the moment
  the memcg is created, so a policy that names a node without memory is valid
  and takes effect once memory appears.
* **Recursion and nesting.** A callback cannot re-enter reclaim itself: it
  cannot sleep, and BPF map allocations never do direct reclaim. A callback
  *can* be interrupted by another callback on the same CPU, for example
  `folio_accessed` from an interrupt while `folio_scan` runs (struct_ops
  programs have no trampoline recursion guard). That is memory-safe, since
  BPF programs are reentrant, but a policy that keeps per-CPU scratch state
  must tolerate it.

## 6. Victim nomination: a pfn is only a suggestion

A policy that nominates victims hands the kernel numbers, never pointers.
Each pfn goes through the checks compaction uses for pages it does not own:

1. `pfn_to_online_page()`: the pfn is in online memory (garbage, holes, device
   memory and offline sections return NULL);
2. `folio_get_nontail_page()`: take a reference, which fails for free pages
   (refcount 0) and tail pages, so the folio cannot be freed or merged into
   a larger one while it is examined;
3. `folio_test_lru()`: it is on an LRU list, i.e. a user folio (only then is
   its memcg read);
4. `folio_lruvec() == lruvec`: it belongs to the lruvec being reclaimed, so a
   policy can never make another cgroup, or another node, lose memory;
5. the restrictions of this reclaim pass (zone, may-swap, may-unmap,
   unevictable), exactly as the engine's own scanner applies them;
6. `folio_isolate_lru()`, which handles races with every other isolator.

What passes is reclaimed by the ordinary `shrink_folio_list()`: locking,
writeback, swap allocation, demotion and freeing are the kernel's code. The
`garbage` test policy nominates 20,000 random, low and swept pfns per run:
all are rejected or are the cgroup's own folios, another cgroup's page cache
is untouched, and reclaim of the policy's cgroup still meets its limit.

**Reclaim can only increase.** Nominated pages are taken out of the engine's
scan budget, but only after they were reclaimed or tried; a policy that
nominates nothing, or garbage, leaves the engine its whole budget. At
priority 0 the engine gets its full budget in any case.

**Every loop is bounded.** At most 32 pfns per call (the kfunc refuses more
and the kernel caps `nr` again). The batch loop stops when the scan budget
is used, the reclaim target is met, a fatal signal arrives, or a batch
yields nothing; `TIER_EXT_AGAIN` extends that by at most 16 empty batches,
and `TIER_EXT_AGE` runs the engine's aging at most once per pass.

**The kfunc is confined.** `bpf_tier_ext_add_victim()` is only available to
`tier_ext_ops.pick_victims` programs (a kfunc filter checks the struct_ops
type and member) and only writes inside the context it is given.

## 7. Engine policies: locks and arena memory

Engine policies hold a `bpf_spin_lock` while they change their lists. That
lock disables interrupts, and the verifier forbids helper calls and anything
that could sleep while it is held; the only kernel function called under it
is `bpf_tier_ext_add_victim()`, which takes no locks. So the lock nests
inside whatever the kernel holds when it calls the policy (for
`folio_added`, the LRU lock) and never the other way round: no lock
inversion is possible.

Their per-pfn nodes live in a BPF arena. A policy with a bug can corrupt its
own lists, but arena memory is separate from kernel memory: an access
outside the arena's allocated pages is caught by the BPF exception handler
(reads see 0, writes are dropped). Corrupt lists only produce bad
nominations, which section 6 makes harmless.

## 8. Cost when unused

All hook sites are behind a static key (`tier_ext_attached`) that is off until
the first policy attaches, plus a per-lruvec pointer test. Without
`CONFIG_TIER_EXT` the hooks compile away entirely.
