#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Milestone 1's byte check: does each level's load consume exactly the bytes sly1-decomp's
docs/DISC.md gives? A level file is LZ-compressed; the loaders read the decompressed image up to
the end of its texture data, and the last 26 to 747 bytes, a copy of the bytes 0x1FFF before them,
are never read. This decompresses every level from the player's disc (the decoder written from
the decompiled UnpackBufferedStream, block for block) and checks the state --verify-levels prints
when a load closes its stream: all compressed bytes read and unpacked, nothing carried, and the
unread decompressed bytes exactly such a tail.

    python3 tools/level_bytes.py --iso DISC.iso LOG [LOG ...]

The logs are sly1_headless --verify-levels, --level or --levels runs (tools/verify_parallel.py
writes them to build-host/verify/). A level loaded right after itself keeps its texture block and
stops before the texture data, as retail does; the check takes the load that read it, from any log.
"""
import argparse
import re
import struct
import sys

SECTOR = 2048
LEVELS = 0x247AF0      # the level table: 46 records of 0x2C bytes, the disc record first
COUNT = 46


def iso_file(f, name):
    f.seek(16 * SECTOR)
    pvd = f.read(SECTOR)
    root = pvd[156:190]
    lba, size = struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0]
    f.seek(lba * SECTOR)
    d = f.read(size)
    pos = 0
    while pos < len(d):
        n = d[pos]
        if n == 0:
            pos = (pos // SECTOR + 1) * SECTOR
            continue
        rec = d[pos:pos + n]
        if rec[33:33 + rec[32]].decode("latin-1").startswith(name):
            flba, fsize = struct.unpack_from("<I", rec, 2)[0], struct.unpack_from("<I", rec, 10)[0]
            f.seek(flba * SECTOR)
            return f.read(fsize)
        pos += n
    raise SystemExit("%s not found on the image" % name)


def elf_reader(elf):
    phoff, = struct.unpack_from("<I", elf, 28)
    phnum, = struct.unpack_from("<H", elf, 44)
    segs = []
    for i in range(phnum):
        t, off, va, pa, fs, ms = struct.unpack_from("<6I", elf, phoff + i * 32)
        if t == 1:
            segs.append((va, off, fs))

    def read(addr, n):
        for va, off, fs in segs:
            if va <= addr and addr + n <= va + fs:
                return elf[off + addr - va: off + addr - va + n]
        raise ValueError(hex(addr))
    return read


def unpack(data):
    """The decompressed image, as UnpackBufferedStream produces it: 0x2000-byte blocks in a window
    of that size (plus the overflow it carries), a flag byte per eight items, a literal or a
    two-byte copy of (v >> 13) + 3 bytes from window offset v & 0x1FFF."""
    base = bytearray(0x2000 + 0x200)
    out = bytearray()
    p, n = 0, len(data)
    flags, carry = 0, 0
    while True:
        pos = 0
        if carry > 0:
            base[0:carry] = base[0x2000:0x2000 + carry]
            pos = carry
        more = True
        while pos < 0x2000:
            flags >>= 1
            if not flags & 0x100:
                if p >= n:
                    more = False
                    break
                flags = data[p] + 0xFF00
                p += 1
            if p >= n:
                more = False
                break
            b = data[p]
            p += 1
            if flags & 1:
                base[pos] = b
                pos += 1
            else:
                v = b + (data[p] << 8) if p < n else b
                p += 1
                cnt = (v >> 13) + 2
                src = v & 0x1FFF
                while cnt >= 0:
                    base[pos] = base[src & 0x1FFF]
                    pos += 1
                    src += 1
                    cnt -= 1
        if pos >= 0x2000:
            out += base[0:0x2000]
            carry = pos - 0x2000
        else:
            out += base[0:pos]
            return bytes(out)
        if not more:
            out += base[0x2000:0x2000 + carry]
            return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--iso", required=True)
    ap.add_argument("logs", nargs="+")
    args = ap.parse_args()
    ends = {}
    for path in args.logs:
        for m in re.finditer(r"verify: level +(\d+) stream at close: (-?\d+) decompressed bytes unread, (-?\d+) "
                             r"compressed bytes unread, (-?\d+) read but not unpacked, (-?\d+) carried"
                             r"(?:, texture block -?\d+ bytes (read|kept))?",
                             open(path, errors="replace").read()):
            # a load whose texture block is kept from the last load (the same key, the same level
            # loaded again) stops before the texture data, retail's own path; the check needs a
            # load that reads it, from any log
            level, kept = int(m.group(1)), m.group(6) == "kept"
            if kept and level in ends and not ends[level][1]:
                continue
            ends[level] = (tuple(int(x) for x in m.groups()[1:5]), kept)
    with open(args.iso, "rb") as f:
        read = elf_reader(iso_file(f, "SCUS_971.98"))
        bad = 0
        for i in range(COUNT):
            rec = read(LEVELS + i * 0x2C, 0x2C)
            w = struct.unpack_from("<8I", rec)
            sector, size = w[0] ^ w[5], w[1] ^ w[7]
            name_ptr, = struct.unpack_from("<I", rec, 0x24)
            name = read(name_ptr, 64).split(b"\0")[0].decode("latin-1") if name_ptr else "?"
            f.seek(sector * SECTOR)
            image = unpack(f.read(size))
            end = ends.get(i + 1)
            if end is None:
                print("level %2d %-30s no stream state in the logs" % (i + 1, name))
                bad += 1
                continue
            (unread, compressed, pending, carried), kept = end
            if kept:
                print("level %2d %-30s not checked: its texture block was kept from the last load, "
                      "run it after another level" % (i + 1, name))
                bad += 1
                continue
            if compressed == 0 and pending <= 0:
                consumed = len(image) - unread
            else:
                # The load stopped with compressed bytes left: the decoder had unpacked whole blocks
                # (nothing carried), so it had read k * 0x2000 bytes, `unread` of them not taken;
                # the one k that leaves a tail of 26 to 747 bytes.
                ks = [k for k in range(len(image) // 0x2000 + 2)
                      if 26 <= len(image) - (k * 0x2000 - unread) <= 747]
                consumed = ks[0] * 0x2000 - unread if ks and carried == 0 else -1
                if consumed < 0:
                    consumed = len(image) - unread
            unread = len(image) - consumed
            # The tail starts where the texture data ends; with no texture data, at the multiple of 16
            # after the objects, the padding before it unread too.
            start = consumed
            def repeated(s):
                return all(image[k] == image[k - 0x1FFF] for k in range(s, len(image)))
            repeats = repeated(start)
            if not repeats and consumed % 16:
                start = (consumed + 15) & ~15
                repeats = repeated(start)
            ok = carried == 0 and 26 <= unread <= 747 and repeats
            bad += not ok
            print("level %2d %-30s %s: %d of %d decompressed bytes read, %d unread%s%s" % (
                i + 1, name, "ok" if ok else "FAILED", consumed, len(image), unread,
                "" if repeats else ", the unread bytes are not the repeated tail",
                "" if compressed == 0 and pending <= 0 else
                " (the last %d compressed bytes not needed)" % (compressed + max(pending, 0))))
        print("\n%d of %d levels consume exactly their bytes" % (COUNT - bad, COUNT))
        return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
