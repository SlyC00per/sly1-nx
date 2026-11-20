#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Names the places SLY1_SAMPLE=1 counted (port/platform/thread.cpp): the "sample:" lines of a
run's log, each an offset in the program's image, summed by the function of the ELF that holds it.

    python3 tools/sample_report.py LOG build-switch/sly1.elf
"""
import bisect
import collections
import subprocess
import sys


def main():
    log, elf = sys.argv[1], sys.argv[2]
    nm = "/opt/devkitpro/devkitA64/bin/aarch64-none-elf-nm"
    symbols = []
    for line in subprocess.run([nm, "-n", "-C", elf], capture_output=True, text=True).stdout.splitlines():
        part = line.split(None, 2)
        if len(part) == 3 and part[1] in "tTwW":
            symbols.append((int(part[0], 16), part[2]))
    starts = [s[0] for s in symbols]
    total = collections.Counter()
    by = collections.defaultdict(collections.Counter)
    for line in open(log, errors="replace"):
        word = line.replace("\r", "").split()
        if len(word) == 4 and word[0] == "sample:" and word[1] in ("render", "gs", "main"):
            at, n = int(word[2], 16), int(word[3])
            k = bisect.bisect_right(starts, at) - 1
            by[word[1]][symbols[k][1] if k >= 0 else "?"] += n
            total[word[1]] += n
    for thread in by:
        print("%s thread: %d samples named" % (thread, total[thread]))
        for name, n in by[thread].most_common(45):
            print("  %5.1f%%  %6d  %s" % (100.0 * n / total[thread], n, name[:110]))


if __name__ == "__main__":
    main()
