#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""The frame rate measured without a player: the same levels, the same stops and the same camera
turn each run, so that two builds can be compared.

    python3 tools/bench.py run NAME [--nro build-switch/sly1.nro] [--levels 4,5,20,4] [--frames 150] [--env NAME=VALUE ...]
    python3 tools/bench.py show LOG [LOG ...]

run: sends the .nro to the Switch through nxlink (the homebrew menu waiting, Y) with --levels, the
tour of each level's arrival points (SLY1_TOUR=arrivals:FRAMES at each), the camera turning
(SLY1_PAD_TURN) and SLY1_PERF_LOG; the log goes to bench/NAME.log, and its summary is printed when
the program ends. No input is needed; the program stops by itself after the last level.

show: the summary of each log: of the one-second windows in 3D (30 parts drawn again a frame or
more, not a load), how many held 60 frames a second, the threads' time a frame, and the frames
shown late. Levels are met more than once on purpose: what a build keeps from level to level
(memory, records) shows in the later visits.

The Switch's address: SLY1_SWITCH (192.168.86.35 when not set)."""
import os
import pty
import re
import select
import signal
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def summary(path):
    d, late = {}, []
    for line in open(path, "rb").read().decode("latin1").replace("\r", "").split("\n"):
        m = re.match(r"(perf\w*): (?:frames )?to (\d+)[:,](.*)", line)
        if m:
            d.setdefault(int(m.group(2)), {})[m.group(1)] = m.group(3)
        m = re.match(r"late: frame (\d+) after .*?(\d+) parts drawn again", line)
        if m:
            late.append((int(m.group(1)), int(m.group(2))))
    n = at60 = 0
    acc = [0.0] * 7
    keys = []
    for k, v in sorted(d.items()):
        if not all(x in v for x in ("perf", "perfgs", "perfjobs")):
            continue
        p = [float(x) for x in re.findall(r"[\d.]+", v["perf"])[:4]]   # frame, render, GS, game
        if p[0] > 40:   # (a load)
            continue
        m = re.search(r"parts drawn again ([\d.]+) ms \((\d+)\)", v["perfgs"])
        if not m or int(m.group(2)) < 30:   # (not in 3D)
            continue
        j = re.search(r"jobs by the helper ([\d.]+) \(([\d.]+) ms\)", v["perfjobs"])
        n += 1
        at60 += p[0] < 17.2
        keys.append(k)
        for i, x in enumerate(p + [float(m.group(2)), float(j.group(1)), float(j.group(2))]):
            acc[i] += x
    if n == 0:
        return "%s: no window in 3D" % os.path.basename(path)
    a = [x / n for x in acc]
    lates = sum(1 for f, parts in late if parts > 50 and any(k - 60 < f <= k for k in keys))
    ended = "ended" if any("verify: all" in l for l in open(path, "rb").read().decode("latin1").split("\n")) else "DID NOT END"
    return ("%s: %d windows in 3D, %.1f%% at 60; a frame %.2f ms, render thread %.2f, GS thread %.2f, game %.2f; "
            "%.0f parts a frame, a helper's job %.0f us; %d frames late (%.0f per 100 windows); %s"
            % (os.path.basename(path), n, 100.0 * at60 / n, a[0], a[1], a[2], a[3], a[4], 1000 * a[6] / max(a[5], 1e-9), lates, 100.0 * lates / n, ended))


def run(name, nro, levels, frames, envs, receiver=False):
    os.makedirs(os.path.join(ROOT, "bench"), exist_ok=True)
    log = os.path.join(ROOT, "bench", name + ".log")
    args = ("--levels %s --env SLY1_LEVEL_FRAMES=120 --env SLY1_TOUR=arrivals:%d --env SLY1_PAD_TURN=300 --env SLY1_PERF_LOG=1"
            % (levels, frames))
    for e in envs:
        args += " --env " + e
    address = os.environ.get("SLY1_SWITCH", "192.168.86.35")
    if receiver:   # (through the receiver, with nobody at the Switch: tools/switch_run.py)
        import switch_run

        def echo(data):
            for line in data.decode("latin1").replace("\r", "").split("\n"):
                if line.startswith(("verify: level", "verify: all", "verify: FAILED", "tour: over", "sly1: fault", "sly1: the run's deadline")):
                    print(line[:110], flush=True)
        status = switch_run.run(nro, log, 3600, args, address, echo)
        if status:
            print("bench: the run did not end as it should (status %d)" % status)
        if status != 2:
            print(summary(log))
        return status
    pid, fd = pty.fork()   # (through a pseudo-terminal: a pipe's output is kept back until nxlink exits)
    if pid == 0:
        os.execv("/opt/devkitpro/tools/bin/nxlink", ["nxlink", "-a", address, "-s", nro, "--args", args])
    end = time.time() + 3600
    tail = b""
    with open(log, "wb") as out:
        while time.time() < end:
            r, _, _ = select.select([fd], [], [], 0.5)
            if r:
                try:
                    data = os.read(fd, 65536)
                except OSError:
                    break
                if not data:
                    break
                out.write(data)
                out.flush()
                tail = (tail + data)[-4096:]
                for line in data.decode("latin1").replace("\r", "").split("\n"):
                    if line.startswith(("verify: level", "verify: all", "verify: FAILED", "tour: over")) or "failed" in line:
                        print(line[:110], flush=True)
                if b"sly1: done, status" in tail:
                    break
            else:
                done, _ = os.waitpid(pid, os.WNOHANG)
                if done:
                    break
    try:
        os.kill(pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    print(summary(log))


def main():
    a = sys.argv[1:]
    if len(a) >= 2 and a[0] == "show":
        for path in a[1:]:
            print(summary(path))
        return
    if len(a) >= 2 and a[0] == "run":
        opt = lambda flag, default: a[a.index(flag) + 1] if flag in a else default
        run(a[1], opt("--nro", os.path.join(ROOT, "build-switch", "sly1.nro")), opt("--levels", "4,5,20,4,5"), int(opt("--frames", "150")),
            [a[k + 1] for k, x in enumerate(a[:-1]) if x == "--env"], "--receiver" in a)
        return
    sys.exit(__doc__)


if __name__ == "__main__":
    main()
