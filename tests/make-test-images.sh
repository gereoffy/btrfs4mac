#!/bin/bash
# SPDX-License-Identifier: MIT
#
# Build btrfs test images with reference manifests. Run as root on Linux
# (needs btrfs-progs, python3, loop device support):
#
#   sudo tests/make-test-images.sh [outdir]
#
# For every variant it writes <outdir>/<name>.img (sparse) and
# <outdir>/<name>.manifest. Copy both into tests/images/ on the Mac
# (sparse aware, e.g. rsync -S, or tar -S) and run "make test".

set -euo pipefail

OUT=${1:-./images}
HERE=$(cd "$(dirname "$0")" && pwd)
MNT=$(mktemp -d)
SIZE=1G
mkdir -p "$OUT"

cleanup() {
	if mountpoint -q "$MNT"; then
		umount "$MNT" 2>/dev/null || umount -l "$MNT"
	fi
	rmdir "$MNT"
}
trap cleanup EXIT
trap 'echo "ERROR: line $LINENO: $BASH_COMMAND" >&2' ERR

# The generators run in process substitutions: the producer dies of SIGPIPE
# when head is done, which would fail the pipeline under pipefail.

# Deterministic pseudo random data
prand() { # <bytes> <seed>
	head -c "$1" < <(openssl enc -aes-128-ctr -pass pass:"$2" -nosalt \
		-pbkdf2 < /dev/zero 2>/dev/null)
}

# Compressible text
ptext() { # <bytes>
	head -c "$1" < <(yes "The quick brown fox jumps over the lazy dog 0123456789")
}

populate() {
	local d=$1

	# Sizes around inline, sector and compression chunk boundaries
	: > "$d/empty"
	echo hello > "$d/tiny"
	for n in 1 100 2047 2048 2049 4095 4096 4097 65536 131071 131072 \
		 131073 1000000; do
		ptext $n > "$d/text-$n"
		prand $n "s$n" > "$d/rand-$n"
	done
	ptext $((64 << 20)) > "$d/text-64M"
	prand $((20 << 20)) big > "$d/rand-20M"

	# Holes (explicit hole extents without no-holes, implicit with it)
	truncate -s 10M "$d/sparse"
	printf 'start' | dd of="$d/sparse" conv=notrunc status=none
	printf 'middle' | dd of="$d/sparse" bs=1 seek=$((5 << 20)) conv=notrunc status=none
	truncate -s 1G "$d/sparse-only"
	ptext 5000 | dd of="$d/sparse-tail" bs=1 seek=$((3 << 20)) status=none

	# Preallocated extents, partially written
	fallocate -l 4M "$d/prealloc"
	ptext 10000 | dd of="$d/prealloc" bs=1 seek=100000 conv=notrunc status=none
	fallocate -n -l 1M -o 8M "$d/prealloc"	# beyond EOF, keep size

	# Many small extents: sync between appends
	for i in $(seq 1 200); do
		ptext 3000 >> "$d/fragmented"
		sync "$d/fragmented"
	done
	# Overwrite in the middle of a compressed/regular extent
	cp "$d/text-1000000" "$d/overwritten"
	sync "$d/overwritten"
	prand 5000 ow | dd of="$d/overwritten" bs=1 seek=300000 conv=notrunc status=none

	# Reflinked copy sharing extents
	cp --reflink=always "$d/rand-1000000" "$d/reflink-copy"

	# Links
	ln -s tiny "$d/symlink"
	ln -s "$(printf 'x%.0s' $(seq 1 2000))" "$d/symlink-long"
	ln -s /nonexistent/target "$d/symlink-dangling"
	ln "$d/text-4096" "$d/hardlink-1"
	mkdir "$d/sub"
	ln "$d/text-4096" "$d/sub/hardlink-2"

	# Special files
	mkfifo "$d/fifo"
	mknod "$d/chardev" c 1 3
	mknod "$d/blockdev" b 7 0

	# Names
	touch "$d/árvíztűrő tükörfúrógép"
	touch "$d/$(printf 'n%.0s' $(seq 1 255))"
	touch "$d/with space" "$d/UPPER" "$d/upper"

	# Large directory: several leaves of dir items
	mkdir "$d/bigdir"
	for i in $(seq 1 5000); do
		: > "$d/bigdir/file-$i"
	done
	# Deep directory
	local p="$d/deep"
	for i in $(seq 1 40); do
		p="$p/level$i"
	done
	mkdir -p "$p"
	echo bottom > "$p/file"

	# Extended attributes
	setfattr -n user.comment -v "hello xattr" "$d/tiny"
	setfattr -n user.big -v "$(ptext 3000)" "$d/tiny"
	setfattr -n user.empty "$d/empty"

	# Permissions, owners, timestamps
	chmod 4755 "$d/rand-100"
	chmod 2711 "$d/rand-1"
	chmod 1777 "$d/sub"
	chown 1234:5678 "$d/text-100"
	touch -d '1999-12-31 23:59:59.123456789' "$d/text-1"
	touch -h -d '2001-02-03 04:05:06' "$d/symlink"
}

make_image() { # <name> <mkfs options> <mount options>
	local name=$1 mkfs_opts=$2 mount_opts=$3
	local img="$OUT/$name.img"

	echo "=== $name: mkfs.btrfs $mkfs_opts, mount -o $mount_opts"
	rm -f "$img"
	truncate -s $SIZE "$img"
	# shellcheck disable=SC2086
	mkfs.btrfs -q -f $mkfs_opts "$img"
	mount -o "loop,$mount_opts" "$img" "$MNT"
	populate "$MNT"
	umount "$MNT"
	mount -o loop,ro "$img" "$MNT"
	python3 "$HERE/linux-manifest.py" "$MNT" > "$OUT/$name.manifest"
	umount "$MNT"
}

make_image plain       ""                     "noatime"
make_image zlib        ""                     "noatime,compress-force=zlib"
make_image lzo         ""                     "noatime,compress-force=lzo"
make_image zstd        ""                     "noatime,compress-force=zstd:3"
make_image zstd-max    ""                     "noatime,compress-force=zstd:15"
make_image holes       "-O ^no-holes"         "noatime"
make_image nodesize4k  "-n 4096"              "noatime,compress=zstd"
make_image nodesize64k "-n 65536"             "noatime,compress=lzo"
make_image meta-single "-m single -d single"  "noatime"
make_image xxhash      "--csum xxhash"        "noatime,compress=zlib"
make_image sha256      "--csum sha256"        "noatime"
make_image blake2      "--csum blake2"        "noatime,compress=zstd"
make_image nodatasum   ""                     "noatime,nodatasum"
make_image mixed-bg    "--mixed"              "noatime"

ls -ls "$OUT"
