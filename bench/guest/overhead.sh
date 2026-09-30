#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Overhead of tier_ext. Runs each workload ROUNDS times under each setup and
# prints one CSV line per run:
#
#   overhead,<kernel>,<setup>,<workload>,<round>,<seconds>
#
# Setups (only "none" on a kernel without tier_ext):
#   none      no policy attached (tier_ext's static key off)
#   other     observe attached to another cgroup (key on, this lruvec has none)
#   observe   observe attached to this cgroup: every callback runs and counts
#   lru, mglru  the BPF engines choose every victim
#
# Workloads (256 MiB cgroup unless noted):
#   pc_reclaim   read 4 x 200 MB files 3 times (page cache, constant reclaim)
#   pc_hit       read a cached 200 MB file 20 times in 4 KiB reads, no limit
#                (the folio_mark_accessed() path)
#   anon_fault   write 1 GiB of fresh anon memory, no limit (lru_add path)
#   anon_reclaim write 512 MiB of anon 3 times in a 256 MiB cgroup (zram swap)
#
# After setting up (attaching, or nothing) every run waits SETTLE seconds
# (-d, default 2), so that deferred work from loading a policy (JIT, text
# patching for the static key, RCU callbacks) is not timed.
#
#   overhead.sh [-r ROUNDS] [-d SETTLE] [-s "none other observe lru mglru"] [-w "..."]

set -u
cd "$(dirname "$0")"
DATA=/var/tmp/tier_ext
CG=/sys/fs/cgroup/bench
OTHER=/sys/fs/cgroup/bench_other
ROUNDS=5 SETTLE=2 SETUPS="none other observe lru mglru"
WORKLOADS="pc_reclaim pc_hit anon_fault anon_reclaim"
while getopts "r:d:s:w:" o; do
	case $o in
	r) ROUNDS=$OPTARG ;;
	d) SETTLE=$OPTARG ;;
	s) SETUPS=$OPTARG ;;
	w) WORKLOADS=$OPTARG ;;
	*) exit 2 ;;
	esac
done
KERNEL=tier_ext
grep -q tier_ext_ops /sys/kernel/btf/vmlinux || { KERNEL=stock; SETUPS=none; }

prepare() {
	echo "+memory" > /sys/fs/cgroup/cgroup.subtree_control
	mkdir -p $DATA
	for f in a b c d; do
		[ -s $DATA/$f ] || dd if=/dev/urandom of=$DATA/$f bs=1M count=200 status=none
	done
	if ! grep -q zram0 /proc/swaps; then
		modprobe zram num_devices=1
		echo 3G > /sys/block/zram0/disksize
		mkswap /dev/zram0 > /dev/null
		swapon -p 100 /dev/zram0
	fi
	mkdir -p $OTHER
}

now() { date +%s.%N; }
elapsed() { awk "BEGIN { printf \"%.3f\", $2 - $1 }"; }

workload() { # workload NAME -> seconds on stdout
	local t0 t1
	case $1 in
	pc_reclaim)
		echo 256M > $CG/memory.max
		t0=$(now)
		sh -c "echo \$\$ > $CG/cgroup.procs; for r in 1 2 3; do cat $DATA/a $DATA/b $DATA/c $DATA/d > /dev/null; done"
		t1=$(now); elapsed "$t0" "$t1" ;;
	pc_hit)
		cat $DATA/a > /dev/null
		sh -c "echo \$\$ > $CG/cgroup.procs; cat $DATA/a > /dev/null"
		t0=$(now)
		sh -c "echo \$\$ > $CG/cgroup.procs; for r in \$(seq 20); do dd if=$DATA/a of=/dev/null bs=4k status=none; done"
		t1=$(now); elapsed "$t0" "$t1" ;;
	anon_fault)
		sh -c "echo \$\$ > $CG/cgroup.procs; exec ./te_touch 1024 1" ;;
	anon_reclaim)
		echo 256M > $CG/memory.max
		sh -c "echo \$\$ > $CG/cgroup.procs; exec ./te_touch 512 3" ;;
	esac
}

run() { # run SETUP WORKLOAD ROUND
	local lp= s
	rmdir $CG 2>/dev/null
	mkdir $CG
	sync; echo 3 > /proc/sys/vm/drop_caches
	case $1 in
	none) ;;
	other) ./tier_ext_load -q -c $OTHER observe.bpf.o > /tmp/oh.out 2>&1 & lp=$! ;;
	*) ./tier_ext_load -q -c $CG "$1.bpf.o" > /tmp/oh.out 2>&1 & lp=$! ;;
	esac
	if [ -n "$lp" ]; then
		for _ in $(seq 50); do grep -q attached /tmp/oh.out && break; sleep 0.1; done
	fi
	sleep "$SETTLE"
	s=$(workload "$2")
	echo "overhead,$KERNEL,$1,$2,$3,$s"
	[ -n "$lp" ] && { kill -INT $lp; wait $lp; }
	rmdir $CG
}

prepare
echo "bench,kernel,setup,workload,round,seconds"
for r in $(seq "$ROUNDS"); do
	for w in $WORKLOADS; do
		for s in $SETUPS; do
			run "$s" "$w" "$r"
		done
	done
done
