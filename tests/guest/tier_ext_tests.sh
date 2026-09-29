#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# tier_ext functional tests. Runs inside the VM as root, from a directory that
# contains tier_ext_load and the policy objects (*.bpf.o).
#
#   ./tier_ext_tests.sh [TEST...]      # default: all tests
#
# Expects a 2-node guest where node 1 is a CPU-less memory node (LinuxDev
# configs/hmat.conf). Prints one PASS/FAIL line per check and exits non-zero
# if any check failed.

set -u
cd "$(dirname "$0")"
HERE=$PWD
LOAD="$HERE/tier_ext_load"
CGROOT=/sys/fs/cgroup
DATA=/var/tmp/tier_ext
DBG=/sys/kernel/debug/tier_ext
FAILS=0
PASSES=0

log()  { echo "[$(date +%T)] $*"; }
pass() { echo "PASS: $*"; PASSES=$((PASSES + 1)); }
fail() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }
check() { # check "description" cond...
	local d=$1; shift
	if "$@"; then pass "$d"; else fail "$d"; fi
}
gt() { [ "$1" -gt "$2" ]; }
lt() { [ "$1" -lt "$2" ]; }
eq() { [ "$1" -eq "$2" ]; }

# ---------------------------------------------------------------- helpers ---

cg_new() { # cg_new NAME [memory.max]
	local cg=$CGROOT/$1
	[ -d "$cg" ] && cg_del "$1"
	mkdir "$cg"
	[ -n "${2:-}" ] && echo "$2" > "$cg/memory.max"
}

cg_del() {
	local cg=$CGROOT/$1 p
	[ -d "$cg" ] || return 0
	for p in $(cat "$cg/cgroup.procs" 2>/dev/null); do kill -9 "$p" 2>/dev/null; done
	sleep 0.3
	rmdir "$cg" 2>/dev/null || { sleep 1; rmdir "$cg" 2>/dev/null; } || true
}

cg_id() { stat -c %i "$CGROOT/$1"; }

cg_run() { # cg_run NAME CMD... : run CMD inside cgroup NAME
	local cg=$CGROOT/$1; shift
	sh -c 'echo $$ > "$0/cgroup.procs" && exec "$@"' "$cg" "$@"
}

# numa_stat CG KEY NODE -> value in KiB
numa_stat() {
	awk -v k="$2" -v n="N$3" '$1 == k { for (i = 2; i <= NF; i++) { split($i, a, "="); if (a[1] == n) print int(a[2] / 1024) } }' \
		"$CGROOT/$1/memory.numa_stat"
}

mem_stat() { awk -v k="$2" '$1 == k { print int($2 / 1024) }' "$CGROOT/$1/memory.stat"; }

vmstat() { awk -v k="$1" '$1 == k { print $2 }' /proc/vmstat; }

# Read file(s) into the page cache of cgroup CG on NODE.
fill_cache() { # fill_cache CG NODE FILE...
	local cg=$1 node=$2; shift 2
	cg_run "$cg" numactl --membind="$node" cat "$@" > /dev/null
}

# Start an anon memory holder of MB megabytes in CG on NODE; sets HOLDER_PID.
start_anon() { # start_anon CG NODE MB
	local ready=/tmp/te_ready.$$
	rm -f "$ready"
	cg_run "$1" numactl --membind="$2" python3 -c "
import mmap, sys, time
n = $3 << 20
m = mmap.mmap(-1, n, flags=mmap.MAP_PRIVATE | mmap.MAP_ANONYMOUS)
for i in range(0, n, 4096):
    m[i] = 1
open('$ready', 'w').close()
time.sleep(100000)
" &
	HOLDER_PID=$!
	for _ in $(seq 300); do [ -e "$ready" ] && break; sleep 0.1; done
	rm -f "$ready"
}

stop_anon() { kill -9 "${HOLDER_PID:-0}" 2>/dev/null; wait "${HOLDER_PID:-0}" 2>/dev/null; HOLDER_PID=; }

# Start the loader in the background; sets LOADER_PID and LOADER_OUT.
start_policy() { # start_policy OBJ LOADER-ARGS...
	local obj=$1; shift
	LOADER_OUT=$(mktemp)
	"$LOAD" "$@" "$HERE/$obj" > "$LOADER_OUT" 2>&1 &
	LOADER_PID=$!
	for _ in $(seq 50); do
		grep -q "attached" "$LOADER_OUT" && return 0
		kill -0 $LOADER_PID 2>/dev/null || break
		sleep 0.1
	done
	cat "$LOADER_OUT"
	return 1
}

stop_policy() { # detach; the loader's variable dump ends up in $OUT
	kill -INT "$LOADER_PID" 2>/dev/null
	wait "$LOADER_PID" 2>/dev/null
	OUT=$(cat "$LOADER_OUT")
	rm -f "$LOADER_OUT"
}

# var DUMP NAME -> value of NAME in a loader dump (0 if absent)
var() { awk -v k="$2" '$1 == k { v = $3 } END { print v + 0 }' <<< "$1"; }

# dbg POLICY STAT -> counter from /sys/kernel/debug/tier_ext
dbg() {
	awk -v p="$1" -v k="$2" '$1 == "policy" { cur = $2 } cur == p && $1 == k { print $2 }' "$DBG"
}

reclaim() { echo "$2" > "$CGROOT/$1/memory.reclaim" 2>/dev/null; true; }

drop_caches() { sync; echo 3 > /proc/sys/vm/drop_caches; }

setup() {
	echo y > /sys/kernel/mm/lru_gen/enabled
	echo 0 > /sys/kernel/mm/numa/demotion_enabled
	log "setup: kernel $(uname -r), lru_gen=$(cat /sys/kernel/mm/lru_gen/enabled)"
	grep -q tier_ext_ops /sys/kernel/btf/vmlinux || { echo "kernel has no tier_ext"; exit 1; }
	[ -e $DBG ] || { echo "no $DBG"; exit 1; }
	echo "+memory" > $CGROOT/cgroup.subtree_control

	# Compressed swap so anon memory is reclaimable.
	if ! grep -q zram0 /proc/swaps; then
		modprobe zram num_devices=1
		echo 3G > /sys/block/zram0/disksize
		mkswap /dev/zram0 > /dev/null
		swapon -p 100 /dev/zram0
	fi

	mkdir -p $DATA
	for f in a b c d; do
		[ -s $DATA/$f ] || dd if=/dev/urandom of=$DATA/$f bs=1M count=200 status=none
	done
	[ -s $DATA/hot ] || dd if=/dev/urandom of=$DATA/hot bs=1M count=40 status=none
	[ -s $DATA/big ] || dd if=/dev/urandom of=$DATA/big bs=1M count=600 status=none
	drop_caches
	dmesg -C
}

# ------------------------------------------------------------------ tests ---

# Attach/detach, debugfs, and that callbacks only fire for the target cgroup.
t_attach() {
	cg_new te_a; cg_new te_b
	start_policy observe.bpf.o -c $CGROOT/te_a || { fail "attach observe"; return; }
	check "debugfs lists the policy" grep -q "policy observe cgroup $(cg_id te_a)" $DBG

	fill_cache te_a 0 $DATA/a
	fill_cache te_b 0 $DATA/b
	reclaim te_a 100M
	reclaim te_b 100M
	stop_policy; local out=$OUT
	echo "$out" | sed 's/^/    /'
	check "folio_added fired for te_a" gt "$(var "$out" 'added[0]')" 1000
	check "folio_scan fired during reclaim" gt "$(var "$out" 'scan[0]')" 0
	check "folio_reclaim fired during reclaim" gt "$(var "$out" 'reclaim[0]')" 0
	check "folio_evicted fired during reclaim" gt "$(var "$out" 'evicted[0]')" 0
	check "lruvec_shrink fired" gt "$(var "$out" 'shrink[0]')" 0
	check "never called for another cgroup" eq "$(var "$out" wrong_target)" 0
	check "debugfs empty after detach" eq "$(grep -c policy $DBG)" 0
	cg_del te_a; cg_del te_b
}

# A policy attached to node 1 only sees node 1's lruvec.
t_per_node() {
	cg_new te_a
	start_policy observe.bpf.o -c $CGROOT/te_a -n 1 || { fail "attach observe -n 1"; return; }
	check "debugfs shows nodes 1" grep -q "policy observe cgroup $(cg_id te_a) nodes 1$" $DBG
	fill_cache te_a 0 $DATA/a
	fill_cache te_a 1 $DATA/b
	reclaim te_a 300M
	stop_policy; local out=$OUT
	echo "$out" | sed 's/^/    /'
	check "node 1 callbacks fired" gt "$(var "$out" 'scan[1]')" 0
	check "node 1 folio_added fired" gt "$(var "$out" 'added[1]')" 1000
	check "no node 0 callbacks" eq "$(( $(var "$out" 'scan[0]') + $(var "$out" 'added[0]') + $(var "$out" 'shrink[0]') ))" 0
	cg_del te_a
}

# One policy per lruvec; bad targets are rejected.
t_attach_rules() {
	cg_new te_a; cg_new te_b
	start_policy observe.bpf.o -c $CGROOT/te_a -n 0 || { fail "attach first"; return; }
	local first=$LOADER_PID first_out=$LOADER_OUT
	if "$LOAD" -q -c $CGROOT/te_a -n 0 "$HERE/swappiness.bpf.o" -- true 2>/tmp/te_err; then
		fail "second policy on the same lruvec must fail"
	else
		check "second policy on the same lruvec: EBUSY" grep -q "busy" /tmp/te_err
	fi
	check "same cgroup, other node: ok" "$LOAD" -q -c $CGROOT/te_a -n 1 "$HERE/swappiness.bpf.o" -- true
	check "other cgroup, same node: ok" "$LOAD" -q -c $CGROOT/te_b -n 0 "$HERE/swappiness.bpf.o" -- true
	LOADER_PID=$first LOADER_OUT=$first_out; stop_policy > /dev/null

	if "$LOAD" -q -i 999999999 "$HERE/observe.bpf.o" -- true 2>/tmp/te_err; then
		fail "unknown cgroup id must fail"
	else
		check "unknown cgroup id: ENOENT" grep -qi "no such file" /tmp/te_err
	fi
	if "$LOAD" -q -c $CGROOT/te_a -n 7 "$HERE/observe.bpf.o" -- true 2>/tmp/te_err; then
		fail "impossible node must fail"
	else
		check "impossible node: EINVAL" grep -qi "invalid" /tmp/te_err
	fi
	mkdir -p $CGROOT/te_a/nomem	# memory controller not enabled below te_a
	if "$LOAD" -q -c $CGROOT/te_a/nomem "$HERE/observe.bpf.o" -- true 2>/tmp/te_err; then
		fail "cgroup without memory controller must fail"
	else
		check "cgroup without memory controller: EINVAL" grep -qi "invalid" /tmp/te_err
	fi
	rmdir $CGROOT/te_a/nomem
	check "attach to root cgroup works" "$LOAD" -q "$HERE/observe.bpf.o" -- true
	check "no policy left attached" eq "$(grep -c policy $DBG)" 0
	cg_del te_a; cg_del te_b
}

# Per-lruvec swappiness. The cgroup holds 300M anon + 200M file.
swappiness_run() { # swappiness_run VALUE|default AMOUNT -> ANON_DELTA FILE_DELTA (KiB)
	cg_new te_a
	start_anon te_a 0 300
	fill_cache te_a 0 $DATA/a
	local a0 f0
	a0=$(mem_stat te_a anon); f0=$(mem_stat te_a file)
	if [ "$1" != default ]; then
		start_policy swappiness.bpf.o -c $CGROOT/te_a -s value="$1" || fail "attach swappiness"
	fi
	reclaim te_a "$2"
	[ "$1" != default ] && stop_policy
	ANON_DELTA=$(( a0 - $(mem_stat te_a anon) ))
	FILE_DELTA=$(( f0 - $(mem_stat te_a file) ))
	log "swappiness=$1 reclaim $2: anon -${ANON_DELTA}KiB file -${FILE_DELTA}KiB"
	stop_anon; cg_del te_a; drop_caches
}

# Asking for more than the file cache forces the kernel to take anon; with
# swappiness=0 on the lruvec it must not (reclaim falls short instead).
t_swappiness() {
	swappiness_run default 300M
	check "default: kernel reclaims anon when file runs out" gt "$ANON_DELTA" 50000
	swappiness_run 0 300M
	check "swappiness=0: no anon reclaimed" lt "$ANON_DELTA" 4096
	check "swappiness=0: file reclaimed" gt "$FILE_DELTA" 150000
}

# protect: node 0's lruvec is skipped, so reclaim comes from node 1.
t_protect() {
	cg_new te_a
	fill_cache te_a 0 $DATA/a
	fill_cache te_a 1 $DATA/b
	local n0 n1
	n0=$(numa_stat te_a file 0); n1=$(numa_stat te_a file 1)
	start_policy protect.bpf.o -c $CGROOT/te_a -n 0 -s min_pages=1000000 || { fail "attach protect"; return; }
	reclaim te_a 100M
	stop_policy; local out=$OUT
	local d0=$(( n0 - $(numa_stat te_a file 0) )) d1=$(( n1 - $(numa_stat te_a file 1) ))
	log "protect: node0 file -${d0}KiB, node1 file -${d1}KiB, skipped=$(var "$out" skipped)"
	check "protect: lruvec skipped" gt "$(var "$out" skipped)" 0
	check "protect: node 0 cache kept" lt "$d0" 8192
	check "protect: node 1 cache reclaimed" gt "$d1" 80000
	cg_del te_a; drop_caches
}

# stream: file folios are reclaimed even though they were referenced.
t_stream() {
	cg_new te_a
	fill_cache te_a 0 $DATA/a; fill_cache te_a 0 $DATA/a; fill_cache te_a 0 $DATA/a
	start_policy stream.bpf.o -c $CGROOT/te_a || { fail "attach stream"; return; }
	reclaim te_a 100M
	local rr sr
	rr=$(dbg stream reclaim_reclaim); sr=$(dbg stream scan_reclaim)
	stop_policy > /dev/null
	check "stream: scan RECLAIM verdicts applied" gt "${sr:-0}" 0
	check "stream: reclaim RECLAIM verdicts applied" gt "${rr:-0}" 0
	cg_del te_a; drop_caches
}

# tier_demote: clean file is dropped (not demoted), anon is demoted, never
# swapped. Attached to both nodes: on node 1 (the last tier) DEMOTE_ONLY means
# "keep", so anon moves down a tier but never leaves memory. (Attached to node
# 0 only, anon demoted to node 1 would be swapped from node 1's lruvec, which
# has no policy - the per-lruvec semantics working as intended.)
demote_run() { # demote_run with|without
	cg_new te_a
	start_anon te_a 0 200
	fill_cache te_a 0 $DATA/a
	local f1 a1
	f1=$(numa_stat te_a file 1); a1=$(numa_stat te_a anon 1)
	[ "$1" = with ] && { start_policy tier_demote.bpf.o -c $CGROOT/te_a || fail "attach tier_demote"; }
	reclaim te_a 350M
	DEM_OVERRIDDEN=$(dbg tier_demote overridden)
	[ "$1" = with ] && stop_policy
	DEM_FILE1=$(( $(numa_stat te_a file 1) - f1 ))
	DEM_ANON1=$(( $(numa_stat te_a anon 1) - a1 ))
	DEM_SWAP=$(( $(cat $CGROOT/te_a/memory.swap.current) >> 10 ))
	log "tier_demote $1: node1 file +${DEM_FILE1}KiB, node1 anon +${DEM_ANON1}KiB, swap ${DEM_SWAP}KiB"
	stop_anon; cg_del te_a; drop_caches
}

t_tier_demote() {
	echo 1 > /sys/kernel/mm/numa/demotion_enabled
	demote_run without
	local base_file=$DEM_FILE1
	demote_run with
	check "tier_demote: baseline demotes file to node 1" gt "$base_file" 50000
	check "tier_demote: policy stops file demotion" lt "$DEM_FILE1" 8192
	check "tier_demote: policy still demotes anon" gt "$DEM_ANON1" 50000
	# ...unless reclaim got desperate and the kernel overrode the policy
	check "tier_demote: policy never swaps anon" \
		test "$DEM_SWAP" -lt 1024 -o "${DEM_OVERRIDDEN:-0}" -gt 0
	echo 0 > /sys/kernel/mm/numa/demotion_enabled
}

# pin: a policy that refuses every eviction must not wedge a cgroup at its limit.
t_pin() {
	cg_new te_a 128M
	start_policy pin.bpf.o -c $CGROOT/te_a -s skip_lruvec=1 || { fail "attach pin"; return; }
	local t0=$SECONDS rc
	cg_run te_a timeout 120 python3 -c "
import mmap
n = 400 << 20
m = mmap.mmap(-1, n, flags=mmap.MAP_PRIVATE | mmap.MAP_ANONYMOUS)
for i in range(0, n, 4096):
    m[i] = 1
" ; rc=$?
	local ov; ov=$(dbg pin overridden)
	stop_policy > /dev/null
	log "pin: allocator exited rc=$rc after $((SECONDS - t0))s, overridden=$ov, oom_kill=$(awk '$1=="oom_kill"{print $2}' $CGROOT/te_a/memory.events)"
	check "pin: allocation did not hang" test "$rc" -ne 124
	check "pin: kernel overrode KEEP/SKIP at priority 0" gt "${ov:-0}" 0
	check "pin: memory.max respected" lt "$(( $(cat $CGROOT/te_a/memory.peak) >> 20 ))" 130
	cg_del te_a
}

# lfu: hot page cache survives a streaming read in the same cgroup.
lfu_run() { # lfu_run with|without -> HOT_RES (percent of hot file resident)
	cg_new te_a 200M
	[ "$1" = with ] && { start_policy lfu.bpf.o -c $CGROOT/te_a || fail "attach lfu"; }
	for _ in 1 2 3 4 5 6 7 8; do fill_cache te_a 0 $DATA/hot; done
	fill_cache te_a 0 $DATA/big
	HOT_RES=$(fincore -b -n -o RES $DATA/hot | awk '{ print int($1 * 100 / 41943040) }')
	if [ "$1" = with ]; then
		stop_policy; local out=$OUT
		LFU_KEEP=$(var "$out" nr_keep)
		LFU_COLD=$(var "$out" nr_evict_cold)
	fi
	log "lfu $1: hot file ${HOT_RES}% resident"
	cg_del te_a; drop_caches
}

t_lfu() {
	lfu_run without; local base=$HOT_RES
	lfu_run with
	check "lfu: KEEP verdicts for hot folios" gt "$LFU_KEEP" 0
	check "lfu: cold folios evicted early" gt "$LFU_COLD" 0
	check "lfu: hot file stays resident (>= 90%)" test "$HOT_RES" -ge 90
	log "lfu: hot residency ${base}% without policy, ${HOT_RES}% with"
}

# Same checks with the classic active/inactive LRU instead of MGLRU.
t_classic() {
	local was; was=$(cat /sys/kernel/mm/lru_gen/enabled)
	echo n > /sys/kernel/mm/lru_gen/enabled
	cg_new te_a
	start_policy observe.bpf.o -c $CGROOT/te_a || { fail "attach observe (classic)"; return; }
	fill_cache te_a 0 $DATA/a
	reclaim te_a 100M
	stop_policy; local out=$OUT
	check "classic: folio_scan from the classic scanner" gt "$(var "$out" 'scan_classic[0]')" 0
	check "classic: no MGLRU scans" eq "$(var "$out" 'scan_mglru[0]')" 0
	check "classic: folio_reclaim fired" gt "$(var "$out" 'reclaim[0]')" 0
	cg_del te_a; drop_caches
	swappiness_run 0 300M
	check "classic swappiness=0: no anon reclaimed" lt "$ANON_DELTA" 4096
	check "classic swappiness=0: file reclaimed" gt "$FILE_DELTA" 150000
	# MGLRU does not age anon just to satisfy a high swappiness while file
	# is available (upstream behaviour), so this half is classic-only.
	swappiness_run 200 150M
	check "classic swappiness=200: anon reclaimed" gt "$ANON_DELTA" 100000
	check "classic swappiness=200: file untouched" lt "$FILE_DELTA" 4096
	t_protect
	echo "$was" > /sys/kernel/mm/lru_gen/enabled
}

# Attach/detach repeatedly under memory pressure from several cgroups.
t_stress() {
	cg_new te_a 256M; cg_new te_b 256M
	cg_run te_a timeout 60 stress-ng --vm 2 --vm-bytes 400M --page-in -q &
	cg_run te_b timeout 60 sh -c "while :; do cat $DATA/big $DATA/a > /dev/null; done" &
	local i end=$((SECONDS + 45)) n=0 ok=1
	while [ $SECONDS -lt $end ]; do
		for p in observe lfu stream swappiness pin; do
			"$LOAD" -q -c $CGROOT/te_a "$HERE/$p.bpf.o" -- sleep 0.3 2>/dev/null || ok=0
			"$LOAD" -q -c $CGROOT/te_b -n 0 "$HERE/$p.bpf.o" -- sleep 0.2 2>/dev/null || ok=0
			n=$((n + 2))
		done
	done
	wait
	check "stress: $n attach/detach cycles under pressure" test $ok -eq 1
	check "stress: no policy left attached" eq "$(grep -c policy $DBG)" 0
	cg_del te_a; cg_del te_b
}

# ------------------------------------------------------------------- main ---

ALL="attach per_node attach_rules swappiness protect stream tier_demote pin lfu classic stress"
setup
for t in ${@:-$ALL}; do
	log "=== $t"
	"t_$t"
done

log "=== kernel log"
if dmesg | grep -E "WARNING|BUG|Oops|RCU stall|soft lockup|hung_task" ; then
	fail "kernel log is clean"
else
	pass "kernel log is clean"
fi
dmesg | grep tier_ext | tail -3

echo "RESULT: $PASSES passed, $FAILS failed"
[ $FAILS -eq 0 ]
