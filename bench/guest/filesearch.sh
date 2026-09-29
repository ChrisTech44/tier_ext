#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# cache_ext's file search benchmark (its Figures 6/9): ripgrep searches a
# Linux source tree 10 times in a cgroup whose memory limit is smaller than
# the tree, swap off. Runs in the guest, as root, from the directory with
# tier_ext_load and the policies. Prints one CSV line per run:
#
#   filesearch,<engine>,<policy>,<iteration>,<seconds>,<pgscan>,<pgsteal>,<refaults>
#
#   filesearch.sh [-e y|n|both] [-i ITERATIONS] [-l LIMIT] [POLICY...]
#   (POLICY "default" means no policy attached)

set -u
cd "$(dirname "$0")"
HERE=$PWD
DATA=/data/linux-src
CG=/sys/fs/cgroup/bench
ENGINES=y ITER=3 LIMIT=1G PASSES=10
while getopts "e:i:l:p:" o; do
	case $o in
	e) ENGINES=$OPTARG ;;
	i) ITER=$OPTARG ;;
	l) LIMIT=$OPTARG ;;
	p) PASSES=$OPTARG ;;
	*) exit 2 ;;
	esac
done
shift $((OPTIND - 1))
POLICIES=${*:-default mru mglru lru fifo s3fifo sampling lhd}
[ "$ENGINES" = both ] && ENGINES="y n"

st() { awk -v k="$1" '$1 == k { print $2 }' $CG/memory.stat; }

run() { # run ENGINE POLICY ITERATION
	echo "$1" > /sys/kernel/mm/lru_gen/enabled
	rmdir $CG 2>/dev/null
	mkdir $CG && echo "$LIMIT" > $CG/memory.max
	sync; echo 3 > /proc/sys/vm/drop_caches
	local lp= t0 t1
	if [ "$2" != default ]; then
		./tier_ext_load -q -c $CG "$2.bpf.o" > /tmp/bench_loader.out 2>&1 &
		lp=$!
		for _ in $(seq 50); do grep -q attached /tmp/bench_loader.out && break; sleep 0.1; done
	fi
	t0=$(date +%s.%N)
	sh -c "echo \$\$ > $CG/cgroup.procs; for i in \$(seq $PASSES); do rg -j4 write $DATA > /dev/null; done"
	t1=$(date +%s.%N)
	echo "filesearch,$1,$2,$3,$(awk "BEGIN { printf \"%.2f\", $t1 - $t0 }"),$(st pgscan),$(st pgsteal),$(st workingset_refault_file)"
	[ -n "$lp" ] && { kill -INT $lp; wait $lp; }
	rmdir $CG
}

swapoff -a
echo "bench,engine,policy,iteration,seconds,pgscan,pgsteal,refaults"
for e in $ENGINES; do
	for i in $(seq "$ITER"); do
		for p in $POLICIES; do
			run "$e" "$p" "$i"
		done
	done
done
echo y > /sys/kernel/mm/lru_gen/enabled
