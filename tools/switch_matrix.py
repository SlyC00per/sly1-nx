#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""A comparison of settings on the Switch, through the receiver (docs/RECEIVER.md), interleaved.

    python3 tools/switch_matrix.py NAME [--rounds N] [--frames N] [--levels 4,5,20] VARIANT ...

Each VARIANT is "label" or "label:K=V,K=V" (environment settings passed to the game; a first
variant with none is the one compared against). They are run in turn, round after round (A B C A B
C ...), because two runs of one build differ by about 6 points at 60: a single pair says nothing.
A summary after the last round: each variant's % at 60, thread times and late frames, as mean and
standard error over its rounds, and the difference from the first with its error. Logs:
bench/NAME-<label>-<round>.log.

It stops by itself when the receiver is gone, or a run does not end as it should (status not 0).
"""
import os
import re
import statistics as st
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench  # noqa: E402

ROOT = bench.ROOT


def measures(path):
    text = bench.summary(path)
    m = re.search(r"(\d+) windows in 3D, ([\d.]+)% at 60; a frame ([\d.]+) ms, render thread ([\d.]+), GS thread ([\d.]+), game ([\d.]+); "
                  r"([\d.]+) parts a frame, a helper's job ([\d.]+) us; (\d+) frames late \((\d+) per 100 windows\)", text)
    if not m:
        return None
    return {"windows": int(m[1]), "at60": float(m[2]), "frame": float(m[3]), "render": float(m[4]), "gs": float(m[5]),
            "game": float(m[6]), "parts": float(m[7]), "job_us": float(m[8]), "late": float(m[10])}


def main():
    a = sys.argv[1:]
    if not a:
        sys.exit(__doc__)
    name = a.pop(0)
    opts = {"--rounds": 3, "--frames": 150, "--levels": "4,5,20,4,5"}
    variants = []
    while a:
        x = a.pop(0)
        if x in opts:
            opts[x] = a.pop(0)
            continue
        label, _, envs = x.partition(":")
        variants.append((label, [e for e in envs.split(",") if e]))
    rounds, frames, levels = int(opts["--rounds"]), int(opts["--frames"]), opts["--levels"]
    nro = os.path.join(ROOT, "build-switch", "sly1.nro")
    got = {label: [] for label, _ in variants}
    for r in range(rounds):
        for label, envs in variants:
            log = "%s-%s-%d" % (name, label, r)
            print("== %s" % log, flush=True)
            status = bench.run(log, nro, levels, frames, envs, receiver=True)
            m = measures(os.path.join(ROOT, "bench", log + ".log"))
            if status:
                print("stopped: that run did not end as it should (status %d)" % status)
                report(variants, got)
                sys.exit(1)
            if m:
                got[label].append(m)
            time.sleep(4)   # (the receiver starts again)
    report(variants, got)


def report(variants, got):
    cols = (("at60", "% at 60"), ("frame", "frame ms"), ("render", "render ms"), ("gs", "GS ms"), ("game", "game ms"), ("job_us", "job us"), ("late", "late/100"))
    base = got[variants[0][0]]
    print("\nvariant (rounds): mean +- standard error; the difference from the first, with its error, in the second line")
    for label, envs in variants:
        v = got[label]
        if len(v) < 2:
            print("%s: %d runs" % (label, len(v)))
            continue
        line = "%s (%d):" % (label, len(v))
        diff = "    difference:"
        for key, title in cols:
            x = [m[key] for m in v]
            line += "  %s %.2f +- %.2f" % (title, st.mean(x), st.stdev(x) / len(x) ** 0.5)
            if label != variants[0][0] and len(base) >= 2:
                y = [m[key] for m in base]
                d = st.mean(x) - st.mean(y)
                e = (st.variance(x) / len(x) + st.variance(y) / len(y)) ** 0.5
                diff += "  %s %+.2f +- %.2f%s" % (title, d, e, " *" if abs(d) > 2 * e else "")
        print(line)
        if label != variants[0][0]:
            print(diff)


if __name__ == "__main__":
    main()
