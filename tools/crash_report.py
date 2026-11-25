#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""A report of a fault or a hang from the Switch, with its addresses named.

    python3 tools/crash_report.py REPORT.txt [--elf build-switch/sly1.elf]

The reports are in sdmc:/switch/sly1/crashes/ (docs/CRASHES.md). Each "+0x..." in one is an offset
into the program; the .elf must be the one of the build the report names in its first line.
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ADDR2LINE = os.path.join(os.environ.get("DEVKITPRO", "/opt/devkitpro"), "devkitA64", "bin", "aarch64-none-elf-addr2line")


def names(elf, offsets):
    """offset -> "function (file:line)", innermost inlined function first."""
    if not offsets:
        return {}
    out = subprocess.run([ADDR2LINE, "-f", "-C", "-e", elf] + ["0x%X" % o for o in offsets], capture_output=True, text=True).stdout.split("\n")
    got = {}
    for i, o in enumerate(offsets):
        function, where = out[2 * i], out[2 * i + 1]
        where = re.sub(r" \(discriminator \d+\)", "", where)
        where = os.path.relpath(where.rsplit(":", 1)[0], ROOT) + ":" + where.rsplit(":", 1)[1] if where.startswith("/") else where
        got[o] = "%s (%s)" % (function, where)
    return got


def main():
    a = sys.argv[1:]
    elf = os.path.join(ROOT, "build-switch", "sly1.elf")
    if "--elf" in a:
        i = a.index("--elf")
        elf = a[i + 1]
        del a[i:i + 2]
    if len(a) != 1:
        sys.exit(__doc__)
    text = open(a[0], errors="replace").read()
    offsets = sorted({int(m, 16) for m in re.findall(r"\+0x([0-9A-Fa-f]+)", text)})
    named = names(elf, offsets)
    for line in text.split("\n"):
        print(line)
        if line.startswith("the last lines printed"):
            named = {}   # (the log's own lines hold no addresses of the program)
        found = [int(m, 16) for m in re.findall(r"\+0x([0-9A-Fa-f]+)", line)]
        for o in found:
            if o in named:
                print("        +0x%X  %s" % (o, named[o]))


if __name__ == "__main__":
    main()
