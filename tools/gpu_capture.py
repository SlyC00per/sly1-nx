#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""The frame the Switch's GPU renderer printed (SLY1_GPU_CAPTURE=N, port/switch/gpu.cpp) as a PNG.

    python3 tools/gpu_capture.py RUN.log OUT.png
"""
import struct
import sys
import zlib


def write(out, width, height, rows):
    rows = (rows + [bytes(width * 3)] * height)[:height]
    raw = b"".join(b"\0" + r for r in rows)

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    with open(out, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
    print("%s: %d by %d" % (out, width, height))


def main():
    """Every capture in the log: OUT.png, then OUT-2.png and so on."""
    log, out = sys.argv[1], sys.argv[2]
    width = height = count = 0
    rows = None
    for line in open(log, errors="replace"):
        if not line.startswith("gpucap: "):
            continue
        word = line.split()
        if len(word) < 2:
            continue
        if word[1] == "begin":
            if rows:
                count += 1
                write(out if count == 1 else out.replace(".png", "-%d.png" % count), width, height, rows)
            width, height = int(word[2]), int(word[3])
            rows = []
        elif word[1] != "end" and rows is not None:
            # a row the link cut short is black from there on
            digits = "".join(c for c in word[1] if c in "0123456789abcdef")[:width * 6]
            digits = digits[:len(digits) & ~1]
            rows.append(bytes.fromhex(digits).ljust(width * 3, b"\0"))
    if rows:
        count += 1
        write(out if count == 1 else out.replace(".png", "-%d.png" % count), width, height, rows)
    if not count:
        sys.exit("no capture in %s" % log)


if __name__ == "__main__":
    main()
