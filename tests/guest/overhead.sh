#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Rough tier_ext overhead: time a page-cache-heavy workload in a 256M cgroup
# (cached re-reads hit folio_mark_accessed; a 600M file forces reclaim)
# with no policy, with observe on another cgroup (static key on, pointer
# NULL), and with observe (every callback, counting only) on this cgroup.
cd "$(dirname "$0")"
CG=/sys/fs/cgroup
echo +memory > $CG/cgroup.subtree_control
mkdir -p $CG/ov $CG/other; echo 256M > $CG/ov/memory.max
F=/var/tmp/tier_ext
[ -s $F/big ] || { mkdir -p $F; dd if=/dev/urandom of=$F/big bs=1M count=600 status=none; }
[ -s $F/hot ] || dd if=/dev/urandom of=$F/hot bs=1M count=40 status=none
work() {
	sync; echo 3 > /proc/sys/vm/drop_caches
	local t0; t0=$(date +%s.%N)
	sh -c "echo \$\$ > $CG/ov/cgroup.procs; for i in 1 2 3 4 5 6 7 8 9 10; do cat $F/hot $F/hot $F/hot $F/hot $F/hot > /dev/null; cat $F/big > /dev/null; done"
	echo "$(date +%s.%N) - $t0" | bc
}
for round in 1 2 3; do
	echo "none $(work)"
	echo "other $(./tier_ext_load -q -c $CG/other ./observe.bpf.o -- bash -c "$(declare -f work); CG=$CG F=$F work" 2>/dev/null | tail -1)"
	echo "observe $(./tier_ext_load -q -c $CG/ov ./observe.bpf.o -- bash -c "$(declare -f work); CG=$CG F=$F work" 2>/dev/null | tail -1)"
done
