# btrfs4mac

Read-only btrfs support for macOS. The goal is an FSKit file system
extension (macOS 15+); the first part, implemented here, is a portable C
library that reads btrfs and a command line tool built on it.

## Status

| Part | State |
|------|-------|
| `libbtrfsro` C library | working, tested on the sample images |
| `btrfsro` command line tool | working |
| FSKit extension | not started |

## Scope

Supported:

* single device filesystems, SINGLE / DUP metadata and data (RAID1 family
  and RAID0 when every copy is on the one device)
* the default subvolume as root; other subvolumes show up as directories
* regular files (inline, regular, prealloc, holes, NO_HOLES), directories,
  symlinks, hardlinks, special files, extended attributes
* zlib, lzo and zstd compression
* crc32c, xxhash64, sha256 and blake2b checksums, all metadata verified,
  data checksums verified by default
* any sector size / node size from 4K to 64K

Not supported, by design: writing, multiple devices, RAID5/6, log tree
replay (fsynced changes after the last commit are not visible),
extent-tree-v2 and the raid stripe tree.

## Building

Xcode command line tools are enough:

    make            # build/libbtrfsro.a and build/btrfsro
    make fuzz       # build/fuzz, sanitizer-instrumented corruption fuzzer
    make test       # run tests/run-tests.sh

## The command line tool

    btrfsro [-v] [--no-verify] <command> ...

    info     <image>                      superblock information
    ls       [-l] [-R] <image> [path]     list a directory
    cat      [-o off] [-n len] <image> <path>
    stat     <image> <path>
    readlink <image> <path>
    xattr    <image> <path>
    extract  <image> <path> <destdir>     copy a file or tree out
    manifest <image> [path]               path/metadata/sha256 list
    extents  [-s] <image> <path>          file extent map

`<image>` can be an image file or a disk device (`/dev/rdiskNsM`, needs
read permission, e.g. via sudo).

## Library API

See [core/include/btrfsro.h](core/include/btrfsro.h). The library does no
I/O by itself: the caller passes a read callback, so the FSKit extension
can read through its block device resource. All calls are thread safe after
mount.

## Layout

    core/include/btrfsro.h   public API
    core/src/                btrfs reader (derived from btrfs-fuse)
    core/vendor/zstd/        zstd decompressor (BSD)
    tools/btrfsro.c          command line tool
    tests/                   test suite, fuzzer, Linux helper scripts

## Origin

`core/src` started as a port of Qu Wenruo's
[btrfs-fuse](https://github.com/adam900710/btrfs-fuse) (MIT). Changes:
macOS port, I/O through a callback, single device volume code, hash table +
LRU tree block cache, structural validation of every tree block, bounds
checks on all item parsing, fixed LZO and inline compression bugs, own
MIT implementations of crc32c, blake2b, LZO1X and the list helpers (the
GPL-licensed `libs/` of btrfs-fuse is not used), parent lookup, xattrs.

## Testing

`make test` runs:

* known answer tests of all checksum algorithms
* the empty image `teszt-ures/btrfs-test.img`
* on `teszt-kernel/btrfs-test.img`: a full manifest (every file read and
  hashed), the kernel tarball inside the image decompressed and compared
  member by member (type, mode, mtime, size, sha256, link target) with the
  extracted tree, hardlink and symlink checks, `git fsck --full --strict` on
  the extracted `.git`, random offset reads against a sequential read
* every `tests/images/<name>.img` that has a `<name>.manifest` made on Linux
  is compared against it line by line
* corruption fuzzing: metadata blocks are mutated and their checksum
  recomputed, then the whole tree is walked under ASan/UBSan

More images (compression, checksum types, node sizes, holes, prealloc,
xattrs, special files, ...) can be produced on Linux with
`sudo tests/make-test-images.sh`; copy the results into `tests/images/`.
