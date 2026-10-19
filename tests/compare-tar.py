#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compare a tar archive (read from stdin) against a btrfsro manifest.

usage: btrfsro cat img /x.tar.xz | compare-tar.py <manifest> [--prefix ./]

Checks that every tar member exists in the manifest with the same type,
permission bits, size, mtime (seconds), sha256 / symlink target, and that the
manifest has no entries below the archive's top directories that the archive
does not have.
"""
import hashlib
import sys
import tarfile


def load_manifest(path):
    entries = {}
    with open(path, encoding="utf-8", errors="surrogateescape") as f:
        for line in f:
            p, typ, mode, uid, gid, nlink, size, mtime, ino, content = \
                line.rstrip("\n").split("\t")
            entries[p] = dict(type=typ, mode=int(mode, 8), size=int(size),
                              mtime=int(mtime.split(".")[0]), content=content)
    return entries


def main():
    manifest = load_manifest(sys.argv[1])
    prefix = "./"
    if len(sys.argv) > 3 and sys.argv[2] == "--prefix":
        prefix = sys.argv[3]

    errors = 0
    seen = set()
    tops = set()
    members = 0
    with tarfile.open(fileobj=sys.stdin.buffer, mode="r|*") as tar:
        for m in tar:
            name = m.name.rstrip("/")
            path = prefix + name
            tops.add(prefix + name.split("/")[0])
            members += 1
            seen.add(path)
            e = manifest.get(path)
            if e is None:
                print(f"MISSING {path}")
                errors += 1
                continue
            if m.isdir():
                typ = "d"
            elif m.issym():
                typ = "l"
            elif m.isfile():
                typ = "-"
            else:
                typ = "?"
            problems = []
            if e["type"] != typ:
                problems.append(f"type {e['type']} != {typ}")
            if e["mode"] != (m.mode & 0o7777):
                problems.append(f"mode {e['mode']:o} != {m.mode & 0o7777:o}")
            if e["mtime"] != int(m.mtime):
                problems.append(f"mtime {e['mtime']} != {int(m.mtime)}")
            if typ == "-":
                data = tar.extractfile(m).read()
                if e["size"] != len(data):
                    problems.append(f"size {e['size']} != {len(data)}")
                digest = hashlib.sha256(data).hexdigest()
                if e["content"] != digest:
                    problems.append("sha256 mismatch")
            elif typ == "l":
                if e["content"] != m.linkname:
                    problems.append(f"target {e['content']} != {m.linkname}")
            if problems:
                print(f"DIFF {path}: " + ", ".join(problems))
                errors += 1

    extra = [p for p in manifest
             if p not in seen and any(p == t or p.startswith(t + "/")
                                      for t in tops)]
    for p in sorted(extra):
        print(f"EXTRA {p}")
    errors += len(extra)
    print(f"{members} tar members checked, {errors} problems")
    sys.exit(1 if errors else 0)


if __name__ == "__main__":
    main()
