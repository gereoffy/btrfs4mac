#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Random offset/length reads through 'btrfsro cat -o -n' compared with one
full sequential read of the same file.

usage: random-read.py <btrfsro> <image> <path> [iterations] [seed]
"""
import random
import subprocess
import sys


def cat(tool, image, path, offset=None, length=None):
    cmd = [tool, "cat"]
    if offset is not None:
        cmd += ["-o", str(offset), "-n", str(length)]
    cmd += [image, path]
    return subprocess.run(cmd, check=True, stdout=subprocess.PIPE).stdout


def extent_boundaries(tool, image, path):
    out = subprocess.run([tool, "extents", image, path], check=True,
                         stdout=subprocess.PIPE, text=True).stdout
    bounds = []
    for line in out.splitlines()[:-1]:
        start, length = int(line.split()[0]), int(line.split()[1])
        bounds += [start, start + length]
    return bounds


def main():
    tool, image, path = sys.argv[1:4]
    iterations = int(sys.argv[4]) if len(sys.argv) > 4 else 100
    rng = random.Random(int(sys.argv[5]) if len(sys.argv) > 5 else 1)

    full = cat(tool, image, path)
    size = len(full)
    cases = [(0, 1), (0, 4096), (1, 4095), (4095, 2), (size - 1, 10),
             (size, 10), (size + 4096, 10), (max(0, size - 5000), 100000)]
    for b in extent_boundaries(tool, image, path):
        for delta in (-4097, -1, 0, 1):
            if 0 <= b + delta:
                cases.append((b + delta, 8192 + 3))
    for _ in range(iterations):
        off = rng.randrange(0, max(1, size))
        length = rng.choice([1, 7, 512, 4096, 4097, 65536, 131072 + 17,
                             1 << 20, 3 * (1 << 20) + 5])
        cases.append((off, length))

    errors = 0
    for off, length in cases:
        got = cat(tool, image, path, off, length)
        want = full[off:off + length]
        if got != want:
            print(f"MISMATCH {path} offset {off} length {length}: "
                  f"got {len(got)} bytes, want {len(want)}")
            errors += 1
    print(f"{path}: {len(cases)} reads of {size} byte file, {errors} mismatches")
    sys.exit(1 if errors else 0)


if __name__ == "__main__":
    main()
