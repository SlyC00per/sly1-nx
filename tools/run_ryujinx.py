# SPDX-License-Identifier: AGPL-3.0-or-later
"""Run a Switch homebrew .nro in Ryujinx without its window, unattended.

    python3 tools/run_ryujinx.py build-smoke/sly1_smoke.nro
    python3 tools/run_ryujinx.py build-switch/sly1.nro --timeout 300 --out out/run1
    python3 tools/run_ryujinx.py build-switch/sly1.nro --iso DISC.iso --args "--level 3"

Ryujinx runs headless (--no-gui) with a data folder of its own (default: build-ryujinx/, so a
run never touches the user's own Ryujinx setup). The program reports through the debug-output
channel (svcOutputDebugString), which Ryujinx writes to its log as guest output; this tool
follows that log, prints the program's lines, and stops the emulator when one reads
"<name>: done, status N" (Ryujinx keeps running after a homebrew program exits). It then copies
the emulated SD card's sly1/ folder into --out and converts every .ppm frame there to .png.
The exit status is the program's status, 124 on a timeout, 125 if Ryujinx could not be run.

--iso links the player's disc image to the emulated SD card's switch/sly1/sly1.iso, where the
game looks for it; --args writes switch/sly1/args.txt, the arguments the game reads when it is
given none (an emulated run is given none), and without --args that file is removed.

Ryujinx is found by --ryujinx, then $SLY1_RYUJINX, then the usual install places. Homebrew
needs no firmware and no keys.
"""

import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import time
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DONE = re.compile(r"^(?P<name>[\w.-]+): done, status (?P<status>-?\d+)\s*$")
GUEST = re.compile(r"OutputDebugString: (?P<text>.*)$")


def find_ryujinx(given):
    """The Ryujinx executable: the argument, $SLY1_RYUJINX, or a usual install place."""
    candidates = []
    if given:
        candidates.append(Path(given))
    if os.environ.get("SLY1_RYUJINX"):
        candidates.append(Path(os.environ["SLY1_RYUJINX"]))
    home = Path.home()
    if sys.platform == "darwin":
        for base in (Path("/Applications"), home / "Applications", home / "Downloads"):
            candidates.append(base / "Ryujinx.app")
    elif os.name == "nt":
        for env in ("LOCALAPPDATA", "PROGRAMFILES", "USERPROFILE"):
            if os.environ.get(env):
                candidates.append(Path(os.environ[env]) / "Ryujinx" / "Ryujinx.exe")
    on_path = shutil.which("Ryujinx") or shutil.which("ryujinx")
    if on_path:
        candidates.append(Path(on_path))
    for c in candidates:
        if c.suffix == ".app":
            c = c / "Contents" / "MacOS" / "Ryujinx"
        if c.is_file():
            return c
    return None


def log_dirs(data_dir, extra):
    """Where Ryujinx may write its log files: the data folder, then each OS's usual place."""
    dirs = [Path(d) for d in extra] + [data_dir / "Logs"]
    home = Path.home()
    if sys.platform == "darwin":
        dirs.append(home / "Library" / "Logs" / "Ryujinx")
    elif os.name == "nt":
        if os.environ.get("APPDATA"):
            dirs.append(Path(os.environ["APPDATA"]) / "Ryujinx" / "Logs")
    else:
        dirs.append(home / ".config" / "Ryujinx" / "Logs")
    return dirs


def newest_log(dirs, since):
    best = None
    for d in dirs:
        if not d.is_dir():
            continue
        for f in d.glob("*.log"):
            m = f.stat().st_mtime
            if m >= since and (best is None or m > best[0]):
                best = (m, f)
    return best[1] if best else None


def write_png(path, width, height, rgb):
    """A minimal 8-bit RGB PNG, so frames need no image library."""
    raw = b"".join(b"\0" + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))

    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data
                + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))


def ppm_to_png(src, dst):
    data = src.read_bytes()
    fields = []
    pos = 0
    while len(fields) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        start = pos
        while not data[pos:pos + 1].isspace():
            pos += 1
        fields.append(data[start:pos])
    pos += 1
    if fields[0] != b"P6" or fields[3] != b"255":
        raise ValueError(f"{src}: not an 8-bit binary PPM")
    w, h = int(fields[1]), int(fields[2])
    write_png(dst, w, h, data[pos:pos + w * h * 3])


GDB_SCRIPT = """\
set pagination off
set confirm off
set tcp connect-timeout 30\nset remotetimeout 60\nset architecture aarch64
target remote 127.0.0.1:{port}
continue
echo gdb: stopped\\n
info registers pc lr sp x29
set $i = 0
set $f = $x29
while $i < 16 && $f != 0
  printf "gdb: frame %lx\\n", *(unsigned long*)($f + 8)
  set $f = *(unsigned long*)$f
  set $i = $i + 1
end
detach
"""


def free_port():
    import socket
    with socket.socket() as sock:
        sock.bind(("localhost", 0))
        return sock.getsockname()[1]


def start_gdb(nro, timeout, port):
    """devkitA64's gdb, attached to Ryujinx's stub once it listens; its output goes to gdb.txt next
    to the .nro, the program's own lines say where its image lies."""
    gdb_exe = Path(os.environ.get("DEVKITPRO", "/opt/devkitpro")) / "devkitA64" / "bin" / "aarch64-none-elf-gdb"
    time.sleep(6)   # the stub listens once Ryujinx has loaded the program (give it --wait 20)
    script = nro.parent / "gdb-commands.txt"
    script.write_text(GDB_SCRIPT.replace("{port}", str(port)))
    out = open(nro.parent / "gdb.txt", "w")
    return subprocess.Popen([str(gdb_exe), "-batch", "-x", str(script)], stdout=out, stderr=subprocess.STDOUT)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("nro", type=Path)
    ap.add_argument("--ryujinx", help="the Ryujinx executable or .app")
    ap.add_argument("--data-dir", type=Path, default=ROOT / "build-ryujinx",
                    help="Ryujinx's data folder for these runs (default: build-ryujinx/)")
    ap.add_argument("--log-dir", action="append", default=[],
                    help="another folder to look for Ryujinx's log in")
    ap.add_argument("--out", type=Path, default=None,
                    help="where to copy the SD card's sly1/ folder (default: next to the .nro)")
    ap.add_argument("--timeout", type=float, default=120.0, help="seconds (default 120)")
    ap.add_argument("--docked", action="store_true", help="docked mode (default: handheld)")
    ap.add_argument("--quiet", action="store_true", help="print only the program's lines")
    ap.add_argument("--gdb", action="store_true",
                    help="attach devkitA64's gdb through Ryujinx's GDB stub; when the program stops (a "
                         "fault), print its pc and frame chain, relative to the image the program reports")
    ap.add_argument("--jit", action="store_true", help="Ryujinx's JIT instead of Apple's Hypervisor")
    ap.add_argument("--iso", type=Path, help="the disc image, linked to the SD card's switch/sly1/sly1.iso")
    ap.add_argument("--args", help="the game's arguments, written to the SD card's switch/sly1/args.txt")
    args = ap.parse_args()

    exe = find_ryujinx(args.ryujinx)
    if exe is None:
        print("run_ryujinx: Ryujinx not found; pass --ryujinx or set SLY1_RYUJINX", file=sys.stderr)
        return 125
    nro = args.nro.resolve()
    if not nro.is_file():
        print(f"run_ryujinx: {nro} does not exist", file=sys.stderr)
        return 125

    data_dir = args.data_dir.resolve()
    sd_out = data_dir / "sdcard" / "sly1"
    if sd_out.exists():
        shutil.rmtree(sd_out)
    data_dir.mkdir(parents=True, exist_ok=True)
    game_dir = data_dir / "sdcard" / "switch" / "sly1"
    game_dir.mkdir(parents=True, exist_ok=True)
    if args.iso:
        link = game_dir / "sly1.iso"
        if link.is_symlink() or link.exists():
            link.unlink()
        try:
            os.link(args.iso.resolve(), link)
        except OSError:
            link.symlink_to(args.iso.resolve())
    args_file = game_dir / "args.txt"
    if args.args is not None:
        args_file.write_text(args.args + "\n")
    elif args_file.exists():
        args_file.unlink()

    # Apple's Hypervisor by default (Ryujinx's JIT froze the whole guest here); --jit for the JIT.
    gdb_port = free_port()
    config = data_dir / "Config.json"
    if config.is_file():
        text = config.read_text()
        fixed = re.sub(r'"use_hypervisor":\s*(true|false)', '"use_hypervisor": %s' % ("false" if args.jit else "true"), text)
        fixed = re.sub(r'"enable_gdb_stub":\s*(true|false)', '"enable_gdb_stub": %s' % ("true" if args.gdb else "false"), fixed)
        # a port of its own each run: a Ryujinx stuck exiting keeps the last one
        fixed = re.sub(r'"gdb_stub_port":\s*\d+', '"gdb_stub_port": %d' % gdb_port, fixed)
        fixed = re.sub(r'"debugger_suspend_on_start":\s*(true|false)',
                       '"debugger_suspend_on_start": %s' % ("true" if args.gdb else "false"), fixed)
        if fixed != text:
            config.write_text(fixed)
    cmd = []
    if sys.platform == "darwin" and subprocess.run(["sysctl", "-n", "hw.optional.arm64"], capture_output=True,
                                                   text=True).stdout.strip() == "1":
        # natively on Apple Silicon: a Python built for Intel would start it under Rosetta
        cmd = ["arch", "-arm64"]
    cmd += [str(exe), "--no-gui", "--root-data-dir", str(data_dir),
           "--skip-user-profiles-manager", "--ignore-controller-applet"]
    if not args.docked:
        cmd.append("--disable-docked-mode")
    # The headless build takes these from its command line, not Config.json.
    cmd += ["--use-hypervisor", "false" if args.jit else "true"]
    if args.gdb:
        cmd += ["--enable-gdb-stub", "true", "--gdb-stub-port", str(gdb_port)]   # the program waits (--wait N in its arguments)
    cmd.append(str(nro))

    started = time.time() - 1
    console = open(data_dir / "console.txt", "wb")
    proc = subprocess.Popen(cmd, stdout=console, stderr=subprocess.STDOUT, cwd=str(data_dir))
    dirs = log_dirs(data_dir, args.log_dir)
    log, offset, status, pending = None, 0, None, ""
    gdb = None
    if args.gdb:
        gdb = start_gdb(nro, args.timeout, gdb_port)
    try:
        while time.time() - started < args.timeout:
            if log is None:
                log = newest_log(dirs, started)
                if log and not args.quiet:
                    print(f"run_ryujinx: log {log}")
            if log is not None:
                with open(log, "r", encoding="utf-8", errors="replace") as f:
                    f.seek(offset)
                    pending += f.read()
                    offset = f.tell()
                *lines, pending = pending.split("\n")
                for line in lines:
                    g = GUEST.search(line)
                    if not g:
                        continue
                    text = g.group("text").rstrip()
                    print(text)
                    d = DONE.match(text)
                    if d:
                        status = int(d.group("status"))
                if status is not None:
                    break
            if proc.poll() is not None:
                break
            if gdb is not None and gdb.poll() is not None:
                break   # gdb saw the program stop and has printed where
            time.sleep(0.25)
    finally:
        # Killed outright: asked to end (SIGTERM), Ryujinx can crash on its way out, and macOS's
        # crash reporter then holds the process, with its huge address space, in an exit that
        # does not end until a restart. A kill is never reported.
        if proc.poll() is None:
            proc.kill()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                print(f"run_ryujinx: Ryujinx (pid {proc.pid}) did not end when killed; it is stuck "
                      "exiting and goes only with a restart", file=sys.stderr)
        console.close()

    if gdb is not None:
        if gdb.poll() is None:
            gdb.kill()
        print((nro.parent / "gdb.txt").read_text(errors="replace"))
    out = (args.out or nro.parent / "run").resolve()
    if sd_out.is_dir():
        if out.exists():
            shutil.rmtree(out)
        shutil.copytree(sd_out, out)
        for ppm in sorted(out.rglob("*.ppm")):
            png = ppm.with_suffix(".png")
            ppm_to_png(ppm, png)
            if not args.quiet:
                print(f"run_ryujinx: frame {png}")

    if status is None:
        why = "Ryujinx exited" if proc.returncode not in (None, -15) and time.time() - started < args.timeout else "timed out"
        print(f"run_ryujinx: no 'done' line ({why}); see {log or data_dir / 'console.txt'}",
              file=sys.stderr)
        return 124
    return status


if __name__ == "__main__":
    sys.exit(main())
