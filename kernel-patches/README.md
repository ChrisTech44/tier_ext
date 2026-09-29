# tier_ext kernel patches

The complete tier_ext kernel change, as two patches against **Linux 7.3-rc5**
(unmodified). Together they touch 8 files: 1,193 lines added, 5 removed.

| Patch | What it does |
|---|---|
| `0001-mm-add-tier_ext-per-lruvec-reclaim-policies-in-BPF.patch` | The whole feature: `include/linux/tier_ext.h`, `mm/tier_ext.c`, the reclaim hooks in `mm/vmscan.c` and `mm/folio.c`, the `struct lruvec` field in `include/linux/mmzone.h`, `CONFIG_TIER_EXT` in `mm/Kconfig`/`mm/Makefile` |
| `0002-mm-tier_ext-look-up-lruvecs-without-NODE_DATA.patch` | Small fix: look up per-node lruvecs without `NODE_DATA()`, which can be NULL for possible-but-offline nodes |

Patch 0001 also changes the kernel tree's top-level `.gitignore`. That file
was missing from this copy of the tree, so the patch restores the usual build
ignores. It is not part of the feature. Drop that hunk if your tree already
has upstream's `.gitignore`.

## Applying

```sh
cd /path/to/linux-7.3-rc5          # a git checkout of v7.3-rc5
git am /path/to/tier_ext/kernel-patches/*.patch

# or, without git history:
patch -p1 < 0001-mm-add-tier_ext-per-lruvec-reclaim-policies-in-BPF.patch
patch -p1 < 0002-mm-tier_ext-look-up-lruvecs-without-NODE_DATA.patch
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
* `../tests/run-vm-tests.sh`: the VM test suite (56/56 passing)

## Regenerating

If the kernel changes again, from the tier_ext repo root:

```sh
rm kernel-patches/*.patch
git -C linux format-patch 3f7b39dd7..HEAD -o ../kernel-patches
```

(`3f7b39dd7` is the "Baseline: Linux 7.3-rc5 (unmodified)" commit in `linux/`.)
