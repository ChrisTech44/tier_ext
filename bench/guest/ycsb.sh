#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# cache_ext's YCSB-on-LevelDB benchmark (its Figure 4), scaled to the VM: a
# 20M-record LevelDB (made by setup.sh) in a cgroup with a 1 GiB limit, 4
# client threads, zipfian 0.99 keys. The database is reset before every run.
# Runs in the guest, as root. Prints one CSV line per run:
#
#   ycsb,<engine>,<policy>,<workload>,<iteration>,<ops/s>,<read avg ns>,<read p99 ns>,<pgscan>,<pgsteal>,<refaults>
#
#   ycsb.sh [-e y|n|both] [-i ITERATIONS] [-w "a b c"] [-t SECONDS] [POLICY...]

set -u
cd "$(dirname "$0")"
DATA=/data
CG=/sys/fs/cgroup/bench
ENGINES=y ITER=2 WORKLOADS="a b c f" RUNTIME=60 WARMUP=20 LIMIT=1G NR_ENTRY=${NR_ENTRY:-20000000}
while getopts "e:i:w:t:l:" o; do
	case $o in
	e) ENGINES=$OPTARG ;;
	i) ITER=$OPTARG ;;
	w) WORKLOADS=$OPTARG ;;
	t) RUNTIME=$OPTARG ;;
	l) LIMIT=$OPTARG ;;
	*) exit 2 ;;
	esac
done
shift $((OPTIND - 1))
POLICIES=${*:-default mglru lru s3fifo lhd sampling fifo}
[ "$ENGINES" = both ] && ENGINES="y n"

st() { awk -v k="$1" '$1 == k { print $2 }' $CG/memory.stat; }

config() { # config WORKLOAD -> yaml path
	local src=$DATA/src/My-YCSB/leveldb/config/ycsb_$1.yaml out=/tmp/ycsb_$1.yaml
	[ "$1" = uniform ] && src=$DATA/src/My-YCSB/leveldb/config/uniform.yaml
	sed -e "s|nr_entry: .*|nr_entry: $NR_ENTRY|" \
	    -e "s|data_dir: .*|data_dir: \"$DATA/leveldb-run\"|" \
	    -e "s|nr_thread: .*|nr_thread: 4|" \
	    -e "s|runtime_seconds: .*|runtime_seconds: $RUNTIME|" \
	    -e "s|warmup_runtime_seconds: .*|warmup_runtime_seconds: $WARMUP|" \
	    -e "s|nr_warmup_op: .*|nr_warmup_op: 100000000|" \
	    -e "s|nr_op: .*|nr_op: 1000000000|" \
	    "$src" > $out
	echo $out
}

run() { # run ENGINE POLICY WORKLOAD ITERATION
	local cfg lp= out
	echo "$1" > /sys/kernel/mm/lru_gen/enabled
	rsync -a --delete $DATA/leveldb-db/ $DATA/leveldb-run/
	cfg=$(config "$3")
	rmdir $CG 2>/dev/null
	mkdir $CG && echo "$LIMIT" > $CG/memory.max
	sync; echo 3 > /proc/sys/vm/drop_caches
	if [ "$2" != default ]; then
		./tier_ext_load -q -c $CG "$2.bpf.o" > /tmp/bench_loader.out 2>&1 &
		lp=$!
		for _ in $(seq 50); do grep -q attached /tmp/bench_loader.out && break; sleep 0.1; done
	fi
	out=$(sh -c "echo \$\$ > $CG/cgroup.procs; exec $DATA/bin/run_leveldb $cfg" 2>&1 | grep -v Warm-Up | grep "overall:")
	echo "ycsb,$1,$2,$3,$4,$(echo "$out" | sed -n 's/.*total throughput \([0-9.]*\) ops.*/\1/p'),$(echo "$out" | sed -n 's/.*READ average latency \([0-9.]*\) ns.*/\1/p'),$(echo "$out" | sed -n 's/.*READ p99 latency \([0-9.]*\) ns.*/\1/p'),$(st pgscan),$(st pgsteal),$(st workingset_refault_file)"
	[ -n "$lp" ] && { kill -INT $lp; wait $lp; }
	rmdir $CG
}

swapoff -a
sysctl -q vm.dirty_background_ratio=1 vm.dirty_ratio=30
echo "bench,engine,policy,workload,iteration,ops_per_s,read_avg_ns,read_p99_ns,pgscan,pgsteal,refaults"
for e in $ENGINES; do
	for w in $WORKLOADS; do
		for i in $(seq "$ITER"); do
			for p in $POLICIES; do
				run "$e" "$p" "$w" "$i"
			done
		done
	done
done
echo y > /sys/kernel/mm/lru_gen/enabled
