#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Print the reference manifest of a mounted directory tree.

Run this on Linux on the mounted btrfs; the output must be identical to
'btrfsro manifest <image>' on the unmounted image:

    sudo mount -o ro image.img /mnt
    python3 linux-manifest.py /mnt > image.manifest

One line per entry, tab separated:
    path type mode uid gid nlink size mtime ino content
Directories are listed depth first, entries sorted by byte value of the name.
"""
import hashlib
import os
import stat
import sys

TYPES = {stat.S_IFREG: "-", stat.S_IFDIR: "d", stat.S_IFLNK: "l",
         stat.S_IFCHR: "c", stat.S_IFBLK: "b", stat.S_IFIFO: "p",
         stat.S_IFSOCK: "s"}


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def line(full, rel):
    st = os.lstat(full)
    fmt = stat.S_IFMT(st.st_mode)
    typ = TYPES.get(fmt, "?")
    if typ == "-":
        content = sha256_file(full)
    elif typ == "l":
        content = os.fsdecode(os.readlink(full))
    else:
        content = "-"
    size = 0 if typ == "d" else st.st_size
    mtime = "%d.%09d" % divmod(st.st_mtime_ns, 1000000000)
    out = "\t".join([os.fsdecode(rel), typ, "%04o" % (st.st_mode & 0o7777),
                     str(st.st_uid), str(st.st_gid), str(st.st_nlink),
                     str(size), mtime, str(st.st_ino), content])
    sys.stdout.buffer.write(os.fsencode(out) + b"\n")
    return typ


def walk(full, rel):
    for name in sorted(os.listdir(full)):
        f = os.path.join(full, name)
        r = rel + b"/" + name
        if line(f, r) == "d":
            walk(f, r)


def main():
    root = os.fsencode(sys.argv[1])
    line(root, b".")
    walk(root, b".")


if __name__ == "__main__":
    main()
