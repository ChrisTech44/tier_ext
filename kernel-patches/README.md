# tier_ext kernel patches

The complete tier_ext kernel change, as seven patches against **Linux 7.3-rc5**
(unmodified). Together they touch 10 files: 2,106 lines added, 8 removed.

| Patch | What it does |
|---|---|
| `0001-mm-add-tier_ext-per-lruvec-reclaim-policies-in-BPF.patch` | The first version: `include/linux/tier_ext.h`, `mm/tier_ext.c`, the reclaim hooks in `mm/vmscan.c` and `mm/folio.c`, the `struct lruvec` field in `include/linux/mmzone.h`, `CONFIG_TIER_EXT` in `mm/Kconfig`/`mm/Makefile` |
| `0002-mm-tier_ext-look-up-lruvecs-without-NODE_DATA.patch` | Small fix: look up per-node lruvecs without `NODE_DATA()`, which can be NULL for possible-but-offline nodes |
| `0003-mm-tier_ext-victim-nomination-pick_victims-and-lifec.patch` | Victim nomination (`pick_victims`, `bpf_tier_ext_add_victim()`), pfn validation and isolation, the hand-off of survivors, `TIER_EXT_AGE`; events `folio_removed`, `folio_migrated`, `folio_referenced`, split tails; `data_key` (touches `mm/migrate.c`, `mm/huge_memory.c`) |
| `0004-mm-tier_ext-flush-MGLRU-walk-batches-after-victims-r.patch` | Flush MGLRU's batched generation sizes after victims; report folios freed on putback |
| `0005-mm-tier_ext-let-pick_victims-ask-to-be-called-again-.patch` | `TIER_EXT_AGAIN` |
| `0006-mm-tier_ext-replace-policies-atomically-struct_ops-..patch` | struct_ops `.update` (in-place policy replacement); drop an unneeded LRU drain |
| `0007-mm-tier_ext-no-WARN-when-an-isolated-victim-s-lruvec.patch` | Count a victim whose lruvec changed after isolation as foreign instead of warning |

Patch 0001 also changes the kernel tree's top-level `.gitignore`. That file
was missing from this copy of the tree, so the patch restores the usual build
ignores. It is not part of the feature. Drop that hunk if your tree already
has upstream's `.gitignore`.

## Applying

```sh
cd /path/to/linux-7.3-rc5          # a git checkout of v7.3-rc5
git am /path/to/tier_ext/kernel-patches/*.patch

# or, without git history:
for p in /path/to/tier_ext/kernel-patches/*.patch; do patch -p1 < "$p"; done
```

Then enable the feature and build:

```sh
scripts/config --enable TIER_EXT    # needs BPF_SYSCALL, BPF_JIT, DEBUG_INFO_BTF, MEMCG
make olddefconfig && make -j$(nproc)
```

These patches were checked by applying them to the unmodified baseline, and
the result is identical to the tested kernel in `../linux`.

## Where to go next

* `../design/README.md`: what tier_ext is and why it is built this way
* `../policies/`: loader and example BPF policies (`make -C policies`)
* `../tests/run-vm-tests.sh`: the VM test suite

## Regenerating

If the kernel changes again, from the tier_ext repo root:

```sh
rm kernel-patches/*.patch
git -C linux format-patch 3f7b39dd7..HEAD -o ../kernel-patches
```

(`3f7b39dd7` is the "Baseline: Linux 7.3-rc5 (unmodified)" commit in `linux/`.)
