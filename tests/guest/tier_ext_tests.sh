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

	# Swap, so anon memory is reclaimable: zram (default) or, with
	# TE_SWAP=partition, a partition on the blank disk the host attached.
	if [ "${TE_SWAP:-zram}" = partition ]; then
		local disk=/dev/disk/by-id/virtio-teswap
		[ -b $disk ] || { echo "no swap disk ($disk)"; exit 1; }
		if ! grep -q "^$(readlink -f $disk)1 " /proc/swaps; then
			echo 'type=swap' | sfdisk -q $disk
			udevadm settle
			mkswap "${disk}-part1" > /dev/null
			swapon "${disk}-part1"
		fi
	elif ! grep -q zram0 /proc/swaps; then
		modprobe zram num_devices=1
		echo 3G > /sys/block/zram0/disksize
		mkswap /dev/zram0 > /dev/null
		swapon -p 100 /dev/zram0
	fi
	log "swap: $(tail -n +2 /proc/swaps | awk '{ printf "%s %s(%dMiB) ", $1, $2, $3 / 1024 }')"

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
	check "lfu: used-once folios evicted early" gt "$LFU_COLD" 0
	check "lfu: hot file stays resident (>= 75%)" test "$HOT_RES" -ge 75
	check "lfu: no worse than without the policy" test "$HOT_RES" -ge "$base"
	log "lfu ($(cat /sys/kernel/mm/lru_gen/enabled)): hot residency ${base}% without policy, ${HOT_RES}% with"
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
	# swappiness=200 prefers anon; it is not "anon only". With disk swap,
	# writeback is asynchronous, memory.reclaim overshoots its target
	# (stock behaviour) and moves on to file once anon is exhausted.
	swappiness_run 200 150M
	check "classic swappiness=200: anon reclaimed" gt "$ANON_DELTA" 100000
	check "classic swappiness=200: anon preferred over file" gt "$ANON_DELTA" "$FILE_DELTA"
	t_protect
	t_lfu
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

# ------------------------------------------------- victim nomination ---

# resident FILE -> "first middle last" percentages of FILE in the page cache
resident() { "$HERE/te_resident" "$1" 3; }

# MRU: a file read once that does not fit keeps its *first* part in memory,
# the opposite of every LRU. Impossible with candidate filtering alone.
mru_run() { # mru_run with|without -> MRU_FIRST MRU_LAST
	cg_new te_a 200M
	drop_caches
	[ "$1" = with ] && { start_policy mru.bpf.o -c $CGROOT/te_a || fail "attach mru"; }
	fill_cache te_a 0 $DATA/big
	read -r MRU_FIRST _ MRU_LAST <<< "$(resident $DATA/big)"
	if [ "$1" = with ]; then
		MRU_ISOLATED=$(dbg mru victims_isolated)
		MRU_INVALID=$(dbg mru victims_invalid)
		stop_policy > /dev/null
	fi
	log "mru $1: big file resident first third ${MRU_FIRST}%, last third ${MRU_LAST}%"
	cg_del te_a; drop_caches
}

t_mru() {
	mru_run without; local base_first=$MRU_FIRST base_last=$MRU_LAST
	mru_run with
	check "mru: default keeps the end of the file" gt "$base_last" 50
	check "mru: default drops the start of the file" lt "$base_first" 10
	check "mru: policy keeps the start of the file (>= 50%)" test "$MRU_FIRST" -ge 50
	check "mru: victims were isolated" gt "${MRU_ISOLATED:-0}" 100
	check "mru: no invalid nominations" eq "${MRU_INVALID:-1}" 0
}

# garbage: nominations that are not the lruvec's folios are harmless. te_b's
# page cache must survive te_a's reclaim, te_a must still be reclaimed.
garbage_run() { # garbage_run MODE
	cg_new te_a 200M; cg_new te_b
	fill_cache te_b 0 $DATA/b
	local fb0; fb0=$(mem_stat te_b file)
	start_policy garbage.bpf.o -c $CGROOT/te_a -s mode="$1" || { fail "attach garbage"; return; }
	fill_cache te_a 0 $DATA/big
	fill_cache te_a 0 $DATA/big
	G_INVALID=$(dbg garbage victims_invalid); G_FOREIGN=$(dbg garbage victims_foreign)
	G_ISOLATED=$(dbg garbage victims_isolated); G_NOM=$(dbg garbage victims_nominated)
	G_OTHER=$(( $(dbg garbage victims_skipped) + $(dbg garbage victims_busy) ))
	stop_policy > /dev/null
	G_BFILE=$(( fb0 - $(mem_stat te_b file) ))
	G_PEAK=$(( $(cat $CGROOT/te_a/memory.peak) >> 20 ))
	log "garbage mode=$1: nominated $G_NOM invalid $G_INVALID foreign $G_FOREIGN isolated $G_ISOLATED; te_b file -${G_BFILE}KiB; te_a peak ${G_PEAK}M"
	cg_del te_a; cg_del te_b; drop_caches
}

t_garbage() {
	garbage_run 0
	# (a random pfn can be one of te_a's own folios: that one is a
	# legitimate victim, so isolations are allowed; te_b must not suffer)
	check "garbage: thousands of bad pfns nominated" gt "${G_NOM:-0}" 1000
	check "garbage: every nomination accounted for" \
		eq "$(( G_INVALID + G_FOREIGN + G_ISOLATED + G_OTHER ))" "$G_NOM"
	check "garbage: nearly all rejected as invalid or foreign" \
		gt "$(( G_INVALID + G_FOREIGN ))" "$(( G_NOM * 99 / 100 ))"
	check "garbage: the other cgroup's cache is untouched" lt "$G_BFILE" 4096
	check "garbage: reclaim still keeps te_a at its limit" lt "$G_PEAK" 202
	garbage_run 1
	check "garbage sweep: other cgroups' folios were nominated" gt "${G_FOREIGN:-0}" 0
	check "garbage sweep: the other cgroup's cache is untouched" lt "$G_BFILE" 4096
}

# An engine policy (lru, mglru) reclaims a hot file + stream workload and an
# anon workload larger than the cgroup, on its own victims.
ANON_PY='
import mmap, sys
n = int(sys.argv[1]) << 20
m = mmap.mmap(-1, n, flags=mmap.MAP_PRIVATE | mmap.MAP_ANONYMOUS)
for r in range(3):
    for i in range(0, n, 4096):
        m[i] = r
'
engine_run() { # engine_run POLICY|default -> E_HOT E_ANON_RC E_SWPOUT E_ISO E_INV E_RET E_NOM
	cg_new te_a 200M
	local p=$1
	E_ISO=0 E_INV=0 E_RET=0 E_NOM=0
	[ "$p" != default ] && { start_policy "$p.bpf.o" -c $CGROOT/te_a || { fail "attach $p"; return; }; }
	for _ in 1 2 3 4 5 6 7 8; do fill_cache te_a 0 $DATA/hot; done
	fill_cache te_a 0 $DATA/big
	E_HOT=$(fincore -b -n -o RES $DATA/hot | awk '{ print int($1 * 100 / 41943040) }')
	local sw0; sw0=$(vmstat pswpout)
	cg_run te_a timeout 180 python3 -c "$ANON_PY" 320; E_ANON_RC=$?
	E_SWPOUT=$(( $(vmstat pswpout) - sw0 ))
	if [ "$p" != default ]; then
		E_ISO=$(dbg "$p" victims_isolated); E_INV=$(dbg "$p" victims_invalid)
		E_RET=$(dbg "$p" victims_returned); E_NOM=$(dbg "$p" victims_nominated)
		stop_policy; E_OUT=$OUT
	fi
	log "engine $p ($(cat /sys/kernel/mm/lru_gen/enabled)): hot ${E_HOT}% resident, anon rc=$E_ANON_RC pswpout=$E_SWPOUT, victims nominated $E_NOM isolated $E_ISO returned $E_RET invalid $E_INV"
	cg_del te_a; drop_caches
}

engine_checks() { # engine_checks POLICY
	check "$1: anon workload larger than the cgroup completes" eq "$E_ANON_RC" 0
	check "$1: victims were isolated" gt "$E_ISO" 10000
	# a stale pfn is possible when a folio is freed while someone else has
	# it isolated; it must stay rare
	check "$1: invalid nominations are rare (<0.1%)" test "$E_INV" -le "$(( E_NOM / 1000 ))"
	check "$1: anon was swapped out" gt "${E_SWPOUT:-0}" 20000
}

engines_on() { # engines_on y|n: run default, lru and mglru on one kernel engine
	echo "$1" > /sys/kernel/mm/lru_gen/enabled
	local k; [ "$1" = y ] && k=MGLRU || k=classic
	engine_run default; local base=$E_HOT
	engine_run lru; engine_checks "lru on $k"
	engine_run mglru; engine_checks "mglru on $k"
	check "mglru on $k: hot file protected from the stream (>= 90%)" test "$E_HOT" -ge 90
	check "mglru on $k: page table aging was used" \
		test "$1" = n -o "$(var "$E_OUT" nr_age_requests)" -gt 0
	log "engines on $k kernel: hot residency default ${base}%, mglru ${E_HOT}%"
}

t_engines() {
	local was; was=$(cat /sys/kernel/mm/lru_gen/enabled)
	engines_on y
	engines_on n
	echo "$was" > /sys/kernel/mm/lru_gen/enabled
}

# Page table accesses (folio_referenced): a process keeps touching an
# mmap'd hot file while the cgroup streams a big file and allocates cold
# anon. lfu counts the accesses MGLRU's page table walks report and
# protects the mapped hot folios.
MMAP_PY='
import mmap, sys, time
f = open(sys.argv[1], "rb")
m = mmap.mmap(f.fileno(), 0, prot=mmap.PROT_READ)
end = time.time() + float(sys.argv[2])
while time.time() < end:
    s = 0
    for i in range(0, len(m), 4096):
        s += m[i]
'
lfu_mmap_run() { # lfu_mmap_run with|without -> LM_HOT LM_REF LM_KEEPMAP LM_PT
	echo y > /sys/kernel/mm/lru_gen/enabled
	cg_new te_a 256M
	drop_caches
	LM_REF=0 LM_KEEPMAP=0 LM_PT=0 LM_AGE=0
	[ "$1" = with ] && { start_policy lfu.bpf.o -c $CGROOT/te_a || fail "attach lfu"; }
	cg_run te_a python3 -c "$MMAP_PY" $DATA/hot 40 &
	local toucher=$!
	sleep 2
	fill_cache te_a 0 $DATA/big
	cg_run te_a python3 -c "$ANON_PY" 150
	fill_cache te_a 0 $DATA/big
	LM_HOT=$(resident $DATA/hot | awk '{ print int(($1 + $2 + $3) / 3) }')
	if [ "$1" = with ]; then
		LM_REF=$(dbg lfu folio_referenced)
		stop_policy; LM_KEEPMAP=$(var "$OUT" nr_keep_mapped); LM_PT=$(var "$OUT" nr_pt_access)
		LM_AGE=$(var "$OUT" nr_age_requests)
	fi
	kill $toucher 2>/dev/null; wait $toucher 2>/dev/null
	log "lfu mmap $1: hot mapped file ${LM_HOT}% resident, folio_referenced $LM_REF, KEEP for mapped folios $LM_KEEPMAP"
	cg_del te_a; drop_caches
}

t_lfu_mmap() {
	local was; was=$(cat /sys/kernel/mm/lru_gen/enabled)
	lfu_mmap_run without; local base=$LM_HOT
	lfu_mmap_run with
	check "lfu mmap: aging was requested on demand" gt "${LM_AGE:-0}" 0
	check "lfu mmap: page table walks reported young folios" gt "${LM_REF:-0}" 100
	check "lfu mmap: lfu counted page table accesses" gt "${LM_PT:-0}" 100
	# (MGLRU's own walks keep the mapped hot folios young, so the scanner
	# rarely presents them and KEEP verdicts are not needed here)
	check "lfu mmap: hot mapped file at least as resident as without" \
		test "$LM_HOT" -ge "$(( base - 5 ))"
	echo "$was" > /sys/kernel/mm/lru_gen/enabled
}

# The cache_ext ports on a hot-file + one-pass-scan workload in a 200M
# cgroup: every nomination valid, and each policy's signature behaviour.
port_run() { # port_run POLICY|default -> P_HOT P_REFAULT P_SCAN P_STEAL P_ISO P_INV P_LAST
	cg_new te_a 200M
	drop_caches
	P_ISO=0 P_INV=0
	[ "$1" != default ] && { start_policy "$1.bpf.o" -c $CGROOT/te_a || { fail "attach $1"; return; }; }
	for _ in 1 2 3 4 5 6 7 8; do fill_cache te_a 0 $DATA/hot; done
	fill_cache te_a 0 $DATA/big
	P_HOT=$(resident $DATA/hot | awk '{ print $1 }')
	P_LAST=$(resident $DATA/big | awk '{ print $3 }')
	P_REFAULT=$(awk '$1 == "workingset_refault_file" { print $2 }' $CGROOT/te_a/memory.stat)
	P_SCAN=$(awk '$1 == "pgscan" { print $2 }' $CGROOT/te_a/memory.stat)
	P_STEAL=$(awk '$1 == "pgsteal" { print $2 }' $CGROOT/te_a/memory.stat)
	if [ "$1" != default ]; then
		P_ISO=$(dbg "$1" victims_isolated); P_INV=$(dbg "$1" victims_invalid)
		stop_policy > /dev/null
	fi
	log "port $1: hot ${P_HOT}% resident, big last third ${P_LAST}%, refaults $P_REFAULT, pgscan $P_SCAN pgsteal $P_STEAL, victims isolated $P_ISO invalid $P_INV"
	cg_del te_a; drop_caches
}

t_ports() {
	local p
	port_run default
	for p in fifo s3fifo sampling lhd; do
		port_run $p
		check "$p: victims were isolated" gt "${P_ISO:-0}" 100
		check "$p: no invalid nominations" eq "${P_INV:-1}" 0
		case $p in
		fifo)
			check "fifo: the hot file is not protected (FIFO)" lt "$P_HOT" 20
			check "fifo: the newest data is kept" gt "$P_LAST" 80
			check "fifo: no refaults on a single pass" lt "$P_REFAULT" 1024 ;;
		s3fifo)
			check "s3fifo: the hot file survives the scan (>= 90%)" test "$P_HOT" -ge 90
			check "s3fifo: no refaults on a single pass" lt "$P_REFAULT" 1024
			check "s3fifo: scanning is efficient (pgscan < 1.2 x pgsteal)" \
				lt "$(( P_SCAN * 10 ))" "$(( P_STEAL * 12 ))" ;;
		esac
		# sampling (LFU) and lhd have no cheap signature on this
		# workload: LFU scores unread read-ahead lowest (see the
		# refaults), so they are smoke tests; results are logged.

	done
}

# Lifecycle events: freed folios (folio_removed), migrated folios
# (folio_migrated, followed by folio_added on the new node's lruvec).
t_events() {
	cg_new te_a
	start_policy lru.bpf.o -c $CGROOT/te_a || { fail "attach lru"; return; }
	cp $DATA/a $DATA/tmp_a
	drop_caches	# the copy is charged to us; te_a must read it in
	fill_cache te_a 0 $DATA/tmp_a
	rm -f $DATA/tmp_a
	sleep 0.5
	local removed; removed=$(dbg lru folio_removed)
	check "events: deleting a cached file reports folio_removed" gt "${removed:-0}" 20
	start_anon te_a 0 200
	migratepages "$HOLDER_PID" 0 1
	local mig a1; mig=$(dbg lru folio_migrated); a1=$(numa_stat te_a anon 1)
	check "events: migratepages moved anon to node 1" gt "$a1" 150000
	# folios, not pages: 200M of anon is ~100 THPs plus small folios
	check "events: migration reports folio_migrated" gt "${mig:-0}" 50
	# the policy now reclaims node 1's lruvec from its own victims
	local iso0; iso0=$(dbg lru victims_isolated)
	reclaim te_a 150M
	local iso1 inv; iso1=$(dbg lru victims_isolated); inv=$(dbg lru victims_invalid)
	check "events: migrated folios are reclaimed as victims on node 1" gt "$(( iso1 - iso0 ))" 20
	check "events: no invalid nominations after migration" eq "${inv:-1}" 0
	stop_anon
	stop_policy > /dev/null
	cg_del te_a; drop_caches
}

# Replacing a policy in place (BPF_LINK_UPDATE): the lruvec never runs
# without one, and the new policy takes over immediately.
t_update() {
	cg_new te_a 200M
	LOADER_OUT=$(mktemp)
	"$LOAD" -c $CGROOT/te_a -u "$HERE/mglru.bpf.o" "$HERE/lru.bpf.o" > "$LOADER_OUT" 2>&1 &
	LOADER_PID=$!
	for _ in $(seq 50); do grep -q attached "$LOADER_OUT" && break; sleep 0.1; done
	fill_cache te_a 0 $DATA/big
	check "update: lru attached first" grep -q "policy lru cgroup" $DBG
	kill -USR1 $LOADER_PID
	for _ in $(seq 50); do grep -q "replaced by" "$LOADER_OUT" && break; sleep 0.1; done
	check "update: the loader replaced the policy" grep -q "replaced by mglru" "$LOADER_OUT"
	check "update: debugfs shows mglru on the same cgroup" grep -q "policy mglru cgroup $(cg_id te_a)" $DBG
	check "update: no gap (one policy listed, never detached)" eq "$(grep -c policy $DBG)" 1
	fill_cache te_a 0 $DATA/big
	local iso; iso=$(dbg mglru victims_isolated)
	check "update: the new policy reclaims" gt "${iso:-0}" 100
	stop_policy > /dev/null
	check "update: kernel log shows the replacement" sh -c "dmesg | grep -q \"replaced policy 'lru' with 'mglru'\""
	check "update: nothing attached after exit" eq "$(grep -c policy $DBG)" 0
	cg_del te_a; drop_caches
}

# Attach/detach engine policies under memory pressure, on both nodes.
t_engine_stress() {
	cg_new te_a 256M; cg_new te_b 256M
	cg_run te_a timeout 70 stress-ng --vm 2 --vm-bytes 400M --page-in -q &
	cg_run te_b timeout 70 sh -c "while :; do cat $DATA/big $DATA/a > /dev/null; done" &
	local end=$((SECONDS + 50)) n=0 ok=1 p
	while [ $SECONDS -lt $end ]; do
		for p in mglru lru mru garbage fifo s3fifo sampling lhd; do
			"$LOAD" -q -c $CGROOT/te_a "$HERE/$p.bpf.o" -- sleep 0.4 2>/dev/null || ok=0
			"$LOAD" -q -c $CGROOT/te_b "$HERE/$p.bpf.o" -- sleep 0.3 2>/dev/null || ok=0
			n=$((n + 2))
		done
	done
	wait
	check "engine stress: $n attach/detach cycles under pressure" test $ok -eq 1
	check "engine stress: no policy left attached" eq "$(grep -c policy $DBG)" 0
	cg_del te_a; cg_del te_b
}

# ------------------------------------------------------------------- main ---

ALL="attach per_node attach_rules swappiness protect stream tier_demote pin lfu classic stress mru garbage engines ports lfu_mmap events update engine_stress"
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
