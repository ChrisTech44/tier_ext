#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# One-time preparation of the benchmark data disk (bench/cache/data.qcow2):
# tool packages, cache_ext's LevelDB + My-YCSB built in the guest, a 20M-record
# YCSB database, and a Linux source tree for the file search. Needs network in
# the guest for the first run only. Takes ~10 minutes.
#
#   bench/setup.sh
#
# Sources: CACHE_EXT (default ~/silence/cache_ext) for leveldb/ and My-YCSB/;
# the Linux tree is the baseline commit of linux/ (git archive).

set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
CACHE_EXT=${CACHE_EXT:-$HOME/silence/cache_ext}
SSH="$ROOT/bench/vm.sh ssh"
SCP="scp -q -F ${LINUXDEV:-$HOME/silence/LinuxDev}/state/ssh/config"

"$ROOT/bench/vm.sh" start | tail -1
trap '"$ROOT/bench/vm.sh" stop > /dev/null 2>&1 || true' EXIT
$SCP "$ROOT/bench/guest/setup.sh" tiervm:/tmp/setup.sh
$SSH 'sudo bash /tmp/setup.sh mount'
if ! $SSH 'test -d /data/src/My-YCSB'; then
	tar -C "$CACHE_EXT" --exclude=.git --exclude=build -cf - leveldb My-YCSB |
		$SSH 'sudo tar -C /data/src -xf -'
fi
$SSH 'sudo bash /tmp/setup.sh all'
if ! $SSH 'test -e /data/linux-src/Makefile'; then
	git -C "$ROOT/linux" archive --format=tar 3f7b39dd7 |
		$SSH 'sudo mkdir -p /data/linux-src && sudo tar -C /data/linux-src -xf -'
fi
$SSH 'du -sh /data/linux-src /data/leveldb-db; sync'
