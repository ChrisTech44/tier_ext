#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Boot the LinuxDev VM for benchmarks, with the persistent benchmark data
# disk (bench/cache/data.qcow2, created on first use) attached as
# /dev/disk/by-id/virtio-tedata. The system disk runs in snapshot mode, the
# data disk does not (snapshot=off), so tools and datasets built once in the
# guest survive.
#
#   bench/vm.sh start [KERNEL_IMAGE]   # default: linux/arch/x86/boot/bzImage
#   bench/vm.sh stop
#   bench/vm.sh ssh [CMD]

set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
LINUXDEV=${LINUXDEV:-$HOME/silence/LinuxDev}
TOPO=${VM_TOPOLOGY:-configs/hmat.conf}
DATA=$ROOT/bench/cache/data.qcow2

case "${1:-}" in
start)
	export CUSTOM_KERNEL=${2:-$ROOT/linux/arch/x86/boot/bzImage}
	[ -e "$DATA" ] || qemu-img create -q -f qcow2 "$DATA" 32G
	cd "$LINUXDEV"
	scripts/stop-vm.sh >/dev/null 2>&1 || true
	scripts/run-vm.sh -d -s -c "$TOPO" -- \
		-drive "file=$DATA,if=none,id=tedata,format=qcow2,snapshot=off,cache=writeback" \
		-device virtio-blk-pci,drive=tedata,serial=tedata
	;;
stop)
	cd "$LINUXDEV" && scripts/stop-vm.sh
	;;
ssh)
	shift
	exec ssh -F "$LINUXDEV/state/ssh/config" tiervm "$@"
	;;
*)
	sed -n '3,14p' "$0"
	exit 2
	;;
esac
