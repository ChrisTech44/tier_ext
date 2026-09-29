#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Boot the tier_ext kernel in the LinuxDev VM, copy the policies and the
# guest test suite in, run it, and power the VM off.
#
#   tests/run-vm-tests.sh [TEST...]
#
# Environment: LINUXDEV (default ~/silence/LinuxDev), VM_TOPOLOGY (default
# configs/hmat.conf: node 0 DRAM + CPUs, node 1 CPU-less slower memory),
# SWAP=zram (default: compressed RAM swap) or SWAP=partition (a swap
# partition on a fresh 4G virtual disk attached for this run only).

set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
LINUXDEV=${LINUXDEV:-$HOME/silence/LinuxDev}
TOPO=${VM_TOPOLOGY:-configs/hmat.conf}
export CUSTOM_KERNEL=$ROOT/linux/arch/x86/boot/bzImage

make -C "$ROOT/policies" -s

SSH="ssh -F $LINUXDEV/state/ssh/config tiervm"
SCP="scp -q -F $LINUXDEV/state/ssh/config"

SWAP=${SWAP:-zram}
QEMU_EXTRA=()
if [ "$SWAP" = partition ]; then
	SWAPDIR=$(mktemp -d)
	qemu-img create -q -f qcow2 "$SWAPDIR/swap.qcow2" 4G
	QEMU_EXTRA=(-- -drive "file=$SWAPDIR/swap.qcow2,if=none,id=teswap,format=qcow2"
		-device virtio-blk-pci,drive=teswap,serial=teswap)
fi

cd "$LINUXDEV"
scripts/stop-vm.sh >/dev/null 2>&1 || true
scripts/run-vm.sh -d -s -c "$TOPO" "${QEMU_EXTRA[@]}"
trap 'cd "$LINUXDEV" && scripts/stop-vm.sh >/dev/null 2>&1 || true; rm -rf "${SWAPDIR:-}"' EXIT

$SSH 'mkdir -p /tmp/te'
$SCP "$ROOT"/policies/build/tier_ext_load "$ROOT"/policies/build/te_resident \
	"$ROOT"/policies/build/*.bpf.o \
	"$ROOT"/tests/guest/tier_ext_tests.sh tiervm:/tmp/te/
$SSH "sudo TE_SWAP=$SWAP /tmp/te/tier_ext_tests.sh $*"
