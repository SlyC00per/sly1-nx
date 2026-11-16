#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Which of the locals the generator gave room (tools/gen_pointers.py, roomy_cast) are really
reached past their own size, and by what: every level run on the audit build (cmake
-DSLY1_ROOM_AUDIT=ON), whose guards poison the added bytes, with ASan going on after each report.
The reports, reduced to unique sites, are the places where retail's frame let code reach a
neighbour, which the port must then reproduce (docs/DESIGN.md).

    python3 tools/room_audit.py --iso DISC.iso [--jobs 8] [--levels 1-46] [--save FOLDER]

Logs go to build-audit/audit/level-NN.log; the sites to build-audit/audit/sites.txt.
"""
import argparse
import concurrent.futures
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def levels(spec):
    out = []
    for part in spec.split(","):
        a, _, b = part.partition("-")
        out += range(int(a), int(b or a) + 1)
    return out


def run(binary, iso, save, n, timeout, logdir):
    env = dict(os.environ, ASAN_OPTIONS="halt_on_error=0:abort_on_error=0:detect_leaks=0")
    with tempfile.TemporaryDirectory() as tmp:
        folder = Path(tmp) / "save"
        if save and Path(save).is_dir():
            shutil.copytree(save, folder)
        try:
            p = subprocess.run([str(binary), "--iso", iso, "--save", str(folder), "--level", str(n)],
                               capture_output=True, text=True, errors="replace", timeout=timeout, env=env)
            log = p.stderr + p.stdout
        except subprocess.TimeoutExpired as e:
            log = (e.stderr.decode(errors="replace") if isinstance(e.stderr, bytes) else (e.stderr or ""))
            log += "\nroom audit: the run took over %d s\n" % timeout
    (logdir / ("level-%02d.log" % n)).write_text(log)
    return n, log


def sites(log):
    """(kind, size, accessor, caller, variable) for each use-after-poison report."""
    out = []
    blocks = log.split("ERROR: AddressSanitizer: use-after-poison")[1:]
    for b in blocks:
        m = re.search(r"\n(READ|WRITE) of size (\d+)", b)
        frames = re.findall(r"#\d+ 0x[0-9a-f]+ in (\S+?)(?:\(|\s).*?(\S+\.(?:cpp|h):\d+)", b)
        frames = [f for f in frames if not f[0].startswith(("__asan", "_platform"))]
        var = re.search(r"\[\d+, \d+\) '([^']+)' \(line (\d+)\) <== Memory access", b)
        owner = re.search(r"in frame\s*\n\s*#0 0x[0-9a-f]+ in (\S+?)(?:\(|\s).*?(\S+\.cpp):\d+", b)
        out.append((m.group(1) if m else "?", int(m.group(2)) if m else 0,
                    "%s %s" % frames[0] if frames else "?",
                    "%s %s" % frames[1] if len(frames) > 1 else "",
                    "%s in %s (%s:%s)" % (var.group(1), owner.group(1) if owner else "?",
                                          owner.group(2) if owner else "?", var.group(2)) if var else "?"))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--iso", required=True)
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 2))
    ap.add_argument("--levels", default="1-46")
    ap.add_argument("--save")
    ap.add_argument("--timeout", type=int, default=600)
    ap.add_argument("--binary", default=str(ROOT / "build-audit" / "sly1_headless"))
    args = ap.parse_args()
    logdir = ROOT / "build-audit" / "audit"
    logdir.mkdir(parents=True, exist_ok=True)
    found = {}
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        futures = [pool.submit(run, args.binary, args.iso, args.save, n, args.timeout, logdir)
                   for n in levels(args.levels)]
        for fut in concurrent.futures.as_completed(futures):
            n, log = fut.result()
            s = sites(log)
            print("level %2d: %d reports" % (n, len(s)), flush=True)
            for site in s:
                found.setdefault(site, set()).add(n)
    lines = ["%d sites" % len(found), ""]
    for (kind, size, at, caller, var), lv in sorted(found.items(), key=lambda kv: kv[0][4]):
        lines.append("%s of %d by %s" % (kind, size, at))
        if caller:
            lines.append("    called from %s" % caller)
        lines.append("    into the room of %s" % var)
        lines.append("    levels %s" % ", ".join(str(n) for n in sorted(lv)))
    text = "\n".join(lines) + "\n"
    (logdir / "sites.txt").write_text(text)
    print("\n" + text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
