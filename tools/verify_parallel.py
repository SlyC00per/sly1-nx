#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Every level verified on its own (sly1_headless --level N), several at a time, and the failures
grouped by their first report: the whole list of what stops the levels in one pass, where
--verify-levels stops at the first.

    python3 tools/verify_parallel.py --iso DISC.iso [--jobs 8] [--levels 4-46] [--save FOLDER]

Each run gets its own copy of the save folder (the game may write it). Logs go to
build-host/verify/level-NN.log; the summary to build-host/verify/summary.txt.
"""
import argparse
import concurrent.futures
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def levels(spec):
    out = []
    for part in spec.split(","):
        a, _, b = part.partition("-")
        out += range(int(a), int(b or a) + 1)
    return out


def signature(log):
    """The first sanitizer report or port stop, with the game frames under it."""
    lines = log.splitlines()
    for i, line in enumerate(lines):
        if "runtime error:" in line or "ERROR: AddressSanitizer" in line or line.startswith("port: ") or \
                line.startswith("verify: FAILED"):
            head = re.sub(r"0x[0-9a-f]{6,}", "…", line.split(": ", 1)[-1] if "runtime error" in line else line)
            head = re.sub(r"==\d+==", "", head).strip()
            frames = []
            for f in lines[i + 1:i + 40]:
                m = re.search(r"#\d+ 0x[0-9a-f]+ in (\S+?)(?:\(| ).*?(\S+\.(?:cpp|h):\d+)", f)
                if m and not m.group(1).startswith(("__asan", "_platform", "port::Ptr32", "port::AtAddr")):
                    frames.append("%s %s" % (m.group(1), m.group(2)))
                if len(frames) == 3:
                    break
            return head, tuple(frames)
    return None, ()


def run(binary, iso, save, n, timeout, logdir):
    with tempfile.TemporaryDirectory() as tmp:
        folder = Path(tmp) / "save"
        if save and Path(save).is_dir():
            shutil.copytree(save, folder)
        start = time.time()
        try:
            p = subprocess.run([str(binary), "--iso", iso, "--save", str(folder), "--level", str(n)],
                               capture_output=True, text=True, errors="replace", timeout=timeout)
            log, code = p.stderr + p.stdout, p.returncode
        except subprocess.TimeoutExpired as e:
            log = (e.stderr or b"").decode(errors="replace") if isinstance(e.stderr, bytes) else (e.stderr or "")
            log += "\nverify: FAILED: the run took over %d s\n" % timeout
            code = -1
        (logdir / ("level-%02d.log" % n)).write_text(log)
        ok = code == 0 and "loaded and ran 60 frames" in log
        name = re.search(r"verify: level +\d+/46, code (0x[0-9A-F]+), (.*)", log)
        return n, ok, (name.group(1) + " " + name.group(2)) if name else "?", time.time() - start, log


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--iso", required=True)
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 2))
    ap.add_argument("--levels", default="1-46")
    ap.add_argument("--save", help="a save folder to start each run from")
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--binary", default=str(ROOT / "build-host" / "sly1_headless"))
    args = ap.parse_args()
    logdir = ROOT / "build-host" / "verify"
    logdir.mkdir(parents=True, exist_ok=True)
    results = []
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        futures = [pool.submit(run, args.binary, args.iso, args.save, n, args.timeout, logdir)
                   for n in levels(args.levels)]
        for fut in concurrent.futures.as_completed(futures):
            n, ok, name, secs, log = fut.result()
            results.append((n, ok, name, secs, log))
            print("level %2d %-40s %s (%.0f s)" % (n, name, "ok" if ok else "FAILED", secs), flush=True)
    groups = {}
    for n, ok, name, secs, log in sorted(results):
        if not ok:
            groups.setdefault(signature(log), []).append((n, name))
    lines = ["%d of %d levels pass" % (sum(1 for r in results if r[1]), len(results)), ""]
    for (head, frames), members in sorted(groups.items(), key=lambda g: -len(g[1])):
        lines.append("%d level(s): %s" % (len(members), head))
        for f in frames:
            lines.append("    " + f)
        lines.append("    levels: " + ", ".join("%d (%s)" % m for m in members))
        lines.append("")
    summary = "\n".join(lines)
    (logdir / "summary.txt").write_text(summary)
    print("\n" + summary)
    return 0 if not groups else 1


if __name__ == "__main__":
    sys.exit(main())
