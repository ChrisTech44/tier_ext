#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Prepare the benchmark data disk, in the guest, as root. Idempotent: every
# step is skipped once its result exists on the disk.
#
#   /data/debs        .deb files of the tools (downloaded once, needs network)
#   /data/src         cache_ext's LevelDB and My-YCSB sources (copied in by the host)
#   /data/bin         built LevelDB benchmark binaries (run_leveldb, init_leveldb)
#   /data/linux-src   a Linux source tree (the file search dataset)
#   /data/leveldb-db  the YCSB database (NR_ENTRY records of 16 B keys, 200 B values)
#
# Every boot (snapshot mode) needs "setup.sh mount" to install the tools again.

set -eu
DATA=/data
DISK=/dev/disk/by-id/virtio-tedata
NR_ENTRY=${NR_ENTRY:-20000000}
PKGS="ripgrep fio cmake libyaml-cpp-dev libsnappy-dev libbpf-dev"

mount_data() {
	mkdir -p $DATA
	blkid $DISK > /dev/null 2>&1 || mkfs.ext4 -q -L tedata $DISK
	mountpoint -q $DATA || mount -o noatime $DISK $DATA
	mkdir -p $DATA/debs $DATA/bin $DATA/src
	if ! ls $DATA/debs/*.deb > /dev/null 2>&1; then
		apt-get update -q > /dev/null
		apt-get install -y -q --download-only -o Dir::Cache::archives=$DATA/debs $PKGS > /dev/null
	fi
	dpkg -i $DATA/debs/*.deb > /dev/null 2>&1 || dpkg -i $DATA/debs/*.deb
}

build_leveldb() {
	[ -x $DATA/bin/run_leveldb ] && return
	[ -d $DATA/src/leveldb ] || { echo "copy the sources in first"; exit 1; }
	cd $DATA/src/leveldb
	cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLEVELDB_BUILD_TESTS=OFF \
		-DLEVELDB_BUILD_BENCHMARKS=OFF -DCMAKE_INSTALL_PREFIX=$DATA/leveldb-install > /dev/null
	cmake --build build -j4 > /dev/null
	cmake --install build > /dev/null
	cd $DATA/src/My-YCSB
	cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_CXX_FLAGS="-I$DATA/leveldb-install/include -O2" \
		-DCMAKE_EXE_LINKER_FLAGS="-L$DATA/leveldb-install/lib -L$DATA/leveldb-install/lib64" > /dev/null
	cmake --build build -j4 --target run_leveldb init_leveldb > /dev/null
	cp build/run_leveldb build/init_leveldb $DATA/bin/
}

make_db() {
	[ -e $DATA/leveldb-db/CURRENT ] && return
	mkdir -p $DATA/leveldb-db
	sed -e "s|nr_entry: .*|nr_entry: $NR_ENTRY|" \
	    -e "s|data_dir: .*|data_dir: \"$DATA/leveldb-db\"|" \
	    -e "s|nr_thread: .*|nr_thread: 4|" \
	    $DATA/src/My-YCSB/leveldb/config/generate_db.yaml > $DATA/generate_db.yaml
	$DATA/bin/init_leveldb $DATA/generate_db.yaml
}

case "${1:-all}" in
mount)	mount_data ;;
all)	mount_data; build_leveldb; make_db ;;
esac
df -h $DATA | tail -1
