#!/bin/bash
# SPDX-License-Identifier: MIT
#
# Test suite for libbtrfsro / btrfsro, run with "make test".
#
# Uses the images teszt-ures/btrfs-test.img (fresh mkfs.btrfs) and
# teszt-kernel/btrfs-test.img (kernel tree, tarball, symlink, hardlink, .git,
# large text file). Any further tests/images/*.img with a matching
# tests/images/*.manifest (made by tests/linux-manifest.py on Linux) is
# compared entry by entry.

cd "$(dirname "$0")/.."
B=build/btrfsro
WORK=$(mktemp -d "${TMPDIR:-/tmp}/btrfsro-test.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

pass=0
fail=0

ok()   { echo "PASS: $*"; pass=$((pass + 1)); }
bad()  { echo "FAIL: $*"; fail=$((fail + 1)); }
check() {
	local name=$1
	shift
	if "$@" > "$WORK/out" 2>&1; then
		ok "$name"
	else
		bad "$name"
		sed 's/^/    /' "$WORK/out" | tail -20
	fi
}

check "hash known answer tests" build/hash-vectors

# ---- empty filesystem ----
EMPTY=teszt-ures/btrfs-test.img
if [ -f "$EMPTY" ]; then
	check "empty: mount and info" $B info "$EMPTY"
	check "empty: root directory is empty" \
		bash -c "[ -z \"\$($B ls $EMPTY /)\" ]"
	check "empty: manifest has only the root" \
		bash -c "[ \$($B manifest $EMPTY | wc -l) -eq 1 ]"
	check "empty: missing file gives ENOENT" \
		bash -c "! $B stat $EMPTY /nope 2>&1 | grep -qv 'No such file'"
fi

# ---- kernel tree image ----
K=teszt-kernel/btrfs-test.img
if [ -f "$K" ]; then
	check "kernel: full manifest without errors" \
		bash -c "$B manifest $K > $WORK/kernel.manifest"

	check "kernel: tarball matches extracted tree" \
		bash -c "$B cat $K /linux-7.2.11.tar.xz | python3 -I tests/compare-tar.py $WORK/kernel.manifest"

	check "kernel: hardlink shares inode, nlink 2 and content" bash -c "
		awk -F'\t' '\$1==\"./HAM.mbox.txt\" || \$1==\"./HAM.mbox.txt-link\" {print \$6, \$9, \$10}' $WORK/kernel.manifest > $WORK/links
		[ \$(wc -l < $WORK/links) -eq 2 ] && [ \$(sort -u $WORK/links | wc -l) -eq 1 ] &&
		[ \"\$(cut -d' ' -f1 $WORK/links | head -1)\" = 2 ]"

	check "kernel: symlink target" \
		bash -c "[ \"\$($B readlink $K /linux)\" = linux-7.2.11 ]"

	check "kernel: path through symlink is not followed" \
		bash -c "! $B stat $K /linux/Makefile"

	check "kernel: .git passes git fsck --full --strict" bash -c "
		$B extract $K /.git $WORK/git >/dev/null 2>&1 &&
		git --git-dir=$WORK/git fsck --full --strict"

	for f in /HAM.mbox.txt /linux-7.2.11.tar.xz /linux-7.2.11/MAINTAINERS \
		 /linux-7.2.11/COPYING; do
		check "kernel: random reads $f" \
			python3 -I tests/random-read.py $B "$K" "$f" 40
	done

	check "kernel: data checksums can be disabled" \
		bash -c "[ \"\$($B --no-verify cat $K /linux-7.2.11/COPYING | shasum -a 256)\" = \"\$($B cat $K /linux-7.2.11/COPYING | shasum -a 256)\" ]"
fi

# ---- extra images with a reference manifest from Linux ----
for img in tests/images/*.img; do
	[ -f "$img" ] || continue
	ref=${img%.img}.manifest
	[ -f "$ref" ] || continue
	name=$(basename "$img" .img)
	check "$name: manifest equals the Linux reference" bash -c "
		$B manifest $img > $WORK/$name.manifest &&
		diff -u $ref $WORK/$name.manifest"
done

# ---- corruption fuzzing ----
if [ -x build/fuzz ]; then
	[ -f "$EMPTY" ] && check "fuzz: empty image, 500 seeds" \
		build/fuzz "$EMPTY" 1 500
	[ -f "$K" ] && check "fuzz: kernel image, 300 seeds" \
		build/fuzz "$K" 1 300 2000
	for n in zlib lzo zstd; do
		img=tests/images/$n.img
		[ -f "$img" ] && check "fuzz: $n image, metadata + compressed data" \
			build/fuzz "$img" 1 200 400 data
	done
else
	echo "SKIP: fuzzing (run 'make fuzz' first)"
fi

echo
echo "$pass passed, $fail failed"
[ $fail -eq 0 ]
