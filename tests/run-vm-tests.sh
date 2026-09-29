#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Boot the tier_ext kernel in the LinuxDev VM, copy the policies and the
# guest test suite in, run it, and power the VM off.
#
#   tests/run-vm-tests.sh [TEST...]
#
# Environment: LINUXDEV (default ~/silence/LinuxDev), VM_TOPOLOGY (default
# configs/hmat.conf: node 0 DRAM + CPUs, node 1 CPU-less slower memory).

set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
LINUXDEV=${LINUXDEV:-$HOME/silence/LinuxDev}
TOPO=${VM_TOPOLOGY:-configs/hmat.conf}
export CUSTOM_KERNEL=$ROOT/linux/arch/x86/boot/bzImage

make -C "$ROOT/policies" -s

SSH="ssh -F $LINUXDEV/state/ssh/config tiervm"
SCP="scp -q -F $LINUXDEV/state/ssh/config"

cd "$LINUXDEV"
scripts/stop-vm.sh >/dev/null 2>&1 || true
scripts/run-vm.sh -d -s -c "$TOPO"
trap 'cd "$LINUXDEV" && scripts/stop-vm.sh >/dev/null 2>&1 || true' EXIT

$SSH 'mkdir -p /tmp/te'
$SCP "$ROOT"/policies/build/tier_ext_load "$ROOT"/policies/build/*.bpf.o \
	"$ROOT"/tests/guest/tier_ext_tests.sh tiervm:/tmp/te/
$SSH "sudo /tmp/te/tier_ext_tests.sh $*"
