#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Run one benchmark in the VM and save its CSV output.
#
#   bench/run.sh KERNEL NAME BENCH [ARGS...]
#
#   KERNEL  tier_ext (linux/) or stock (baseline/, the unmodified 7.3-rc5
#           worktree, see bench/README.md), or a path to a bzImage
#   NAME    results go to bench/results/NAME.csv (appended)
#   BENCH   overhead | filesearch | ycsb (bench/guest/BENCH.sh), ARGS go to it
#
# Needs the data disk prepared by bench/setup.sh for filesearch and ycsb.

set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
KERNEL=$1 NAME=$2 BENCH=$3
shift 3
case $KERNEL in
tier_ext) IMAGE=$ROOT/linux/arch/x86/boot/bzImage ;;
stock)    IMAGE=$ROOT/baseline/arch/x86/boot/bzImage ;;
*)        IMAGE=$KERNEL ;;
esac
SSH="$ROOT/bench/vm.sh ssh"
SCP="scp -q -F ${LINUXDEV:-$HOME/silence/LinuxDev}/state/ssh/config"

make -C "$ROOT/policies" -s
"$ROOT/bench/vm.sh" start "$IMAGE" | tail -1
trap '"$ROOT/bench/vm.sh" stop > /dev/null 2>&1 || true' EXIT
$SSH 'mkdir -p /tmp/b'
$SCP "$ROOT"/policies/build/tier_ext_load "$ROOT"/policies/build/*.bpf.o \
	"$ROOT"/policies/build/te_touch "$ROOT"/bench/guest/*.sh tiervm:/tmp/b/
$SSH 'sudo bash /tmp/b/setup.sh mount > /dev/null'
mkdir -p "$ROOT/bench/results"
out=$ROOT/bench/results/$NAME.csv
$SSH "sudo bash /tmp/b/$BENCH.sh $*" | tee -a "$out" | grep -v '^bench,' || true
$SSH 'sudo dmesg | grep -E "WARNING|BUG|Oops|RCU stall|soft lockup" | head -5' || true
