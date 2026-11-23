#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""prof_resolve.py LOG [ELF] [--top N] [--lines NAME]: the prof: lines of a run with SLY1_PROFILE=1 named from the
ELF (build-switch/sly1.elf), as the share of samples in each function (its own code) and in each
function with the one that called it (the link register, which names the caller only where the
function has not called another since it began). --lines NAME: the samples in the functions whose
name has NAME in it, by source line."""
import collections, os, re, subprocess, sys

DEVKIT = os.environ.get("DEVKITPRO", "/opt/devkitpro") + "/devkitA64/bin/aarch64-none-elf-"


def main():
    lines = sys.argv[sys.argv.index("--lines") + 1] if "--lines" in sys.argv else None
    args = [a for k, a in enumerate(sys.argv[1:]) if not a.startswith("--") and sys.argv[k] not in ("--top", "--lines")]
    top = int(sys.argv[sys.argv.index("--top") + 1]) if "--top" in sys.argv else 40
    log = args[0]
    elf = args[1] if len(args) > 1 and not args[1].isdigit() else os.path.join(os.path.dirname(__file__), "..", "build-switch", "sly1.elf")
    nm = subprocess.run([DEVKIT + "nm", "-C", elf], capture_output=True, text=True).stdout
    anchor = None
    for line in nm.splitlines():
        if line.endswith(" port::ProfileThisThread()") or line.endswith(" port::ProfileThisThread(char const*)"):
            anchor = int(line.split()[0], 16)
    if anchor is None:
        sys.exit("ProfileThisThread not in the ELF")
    pairs = []
    for line in open(log, "rb").read().decode("latin1").splitlines():
        if line.startswith("prof:") and ":" in line[5:] and "samples" not in line and "base" not in line:
            for tok in line[5:].split():
                if not re.fullmatch(r"[0-9A-F]+:[0-9A-F]+", tok):   # (another thread's output run into the line)
                    continue
                pc, lr = tok.split(":")
                pairs.append(((int(pc, 16) + anchor) & 0xFFFFFFFFFFFFFFFF, (int(lr, 16) + anchor) & 0xFFFFFFFFFFFFFFFF))
    if not pairs:
        sys.exit("no samples")
    addrs = sorted({a for p in pairs for a in p})
    out = subprocess.run([DEVKIT + "addr2line", "-f", "-C", "-e", elf] + ["0x%x" % a for a in addrs], capture_output=True, text=True).stdout.splitlines()
    name = {}
    for k, a in enumerate(addrs):
        fn = out[2 * k] if 2 * k < len(out) else "?"
        fn = re.sub(r"\(.*", "", fn.replace("(anonymous namespace)", "{anon}")) if fn != "??" else "(outside the program)"
        name[a] = fn
    if lines:
        pcs = collections.Counter(pc for pc, _ in pairs if lines in name[pc])
        where = subprocess.run([DEVKIT + "addr2line", "-i", "-e", elf] + ["0x%x" % a for a in pcs], capture_output=True, text=True).stdout
        # (with -i an address gives its inlined chain: one line each, the innermost first; split by a marker)
        out2 = subprocess.run([DEVKIT + "addr2line", "-e", elf] + ["0x%x" % a for a in pcs], capture_output=True, text=True).stdout.splitlines()
        byLine = collections.Counter()
        for (a, c), at in zip(pcs.items(), out2):
            byLine[re.sub(r" \(discriminator.*", "", at.split("/")[-1])] += c
        total = sum(pcs.values())
        print("%d samples in %s (%.2f%% of all)" % (total, lines, 100.0 * total / len(pairs)))
        for at, c in byLine.most_common(top):
            print("%6.2f%%  %s" % (100.0 * c / len(pairs), at))
        return
    own = collections.Counter(name[pc] for pc, _ in pairs)
    withCaller = collections.Counter((name[pc], name[lr]) for pc, lr in pairs)
    n = len(pairs)
    print("%d samples" % n)
    print("-- by function")
    for fn, c in own.most_common(top):
        print("%6.2f%%  %s" % (100.0 * c / n, fn))
    print("-- by function and the link register's")
    for (fn, by), c in withCaller.most_common(top):
        print("%6.2f%%  %s  <-  %s" % (100.0 * c / n, fn, by))


main()
