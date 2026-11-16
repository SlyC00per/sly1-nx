#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""The game's VU1 microcode translated to C++ (port/render/vu1_runner.h's chunks), for speed.

The microcode is part of the player's executable (retail's .vutext, 0x211E10 to 0x217360), so
this reads it from the disc image at build time and writes the C++ into the build folder only
(it is derived from the game's data: never committed). Each upload the executable holds (an MPG
VIFcode and its instructions) becomes one function: every instruction a call of the core's
upper and lower operations with its encoding as a constant (the same code the interpreter runs,
so the results are the same), branches with known targets inside the chunk as gotos, delay
slots and end bits laid out. What a chunk does not handle (a branch or end bit in a delay slot,
a delay slot past its end) it leaves to the interpreter. At run time an upload whose address and
hash match a chunk installs it (port/render/vu1.cpp).

    python3 tools/vu1_translate.py --iso DISC.iso --out build/gen/vu1_programs.cpp
    python3 tools/vu1_translate.py --empty --out build/gen/vu1_programs.cpp   (no disc: none)
"""
import argparse
import pathlib
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import level_bytes  # noqa: E402  (the disc image's executable)

VUTEXT = (0x211E10, 0x217360)
STATIC_BRANCHES = {0x20, 0x21, 0x28, 0x29, 0x2C, 0x2D, 0x2E, 0x2F}   # B, BAL, IBxx
DYNAMIC_BRANCHES = {0x24, 0x25}                                        # JR, JALR


def fnv(words):
    h = 2166136261
    for w in words:
        h = ((h ^ w) * 16777619) & 0xFFFFFFFF
    return h


def uploads(read):
    """(address, [instruction words]) for each MPG in .vutext."""
    lo, hi = VUTEXT
    words = struct.unpack("<%dI" % ((hi - lo) // 4), read(lo, hi - lo))
    found = []
    i = 0
    while i < len(words):
        w = words[i]
        if (w >> 24) & 0x7F == 0x4A:
            num = (w >> 16) & 0xFF or 256
            addr = w & 0xFFFF
            start = i + 1
            if start % 2:   # the instructions are 64-bit aligned: a NOP pads before them
                start += 1
            if addr < 0x800 and start + num * 2 <= len(words):
                found.append((addr, list(words[start:start + num * 2])))
                i = start + num * 2
                continue
        i += 1
    return found


def kind(lo, hi):
    if hi & 0x80000000:
        return None
    op = (lo >> 25) & 0x7F
    if op in STATIC_BRANCHES:
        return "static"
    if op in DYNAMIC_BRANCHES:
        return "dynamic"
    return None


def target(pc, lo):
    imm = lo & 0x7FF
    if imm & 0x400:
        imm -= 0x800
    return (pc + 1 + imm) & 0x7FF


def body(pc, lo, hi):
    lower = ("{ std::uint32_t i = 0x%08Xu; std::memcpy(&r.s.i, &i, 4); }" % lo if hi & 0x80000000
             else "next = r.Lower(0x%03X, 0x%08Xu, low);" % (pc, lo))
    return ("{ Runner::Out up = r.Upper(0x%03X, 0x%08Xu); Runner::Out low; long next = -1; %s "
            "r.Write(low); r.Write(up); r.cycle++; r.executed++; t = next; }" % (pc, hi, lower))


def chunk(addr, words):
    count = len(words) // 2
    end = addr + count
    ins = {addr + k: (words[2 * k], words[2 * k + 1]) for k in range(count)}
    name = "chunk_%03X_%08X" % (addr, fnv(words))

    def simple(pc):   # a delay slot the chunk can run inline
        if pc not in ins:
            return False
        lo, hi = ins[pc]
        return kind(lo, hi) is None and not hi & 0x40000000

    def go(pc):
        pc &= 0x7FF
        return "goto L%03X;" % pc if addr <= pc < end else "return 0x%03Xu;" % pc

    out = ["static std::uint32_t %s(Runner& r, std::uint32_t pc)" % name, "{", "    long t = -1;",
           "    switch (pc) {"]
    for pc in range(addr, end):
        out.append("    case 0x%03X: goto L%03X;" % (pc, pc))
    out.append("    default: return pc;")
    out.append("    }")
    for pc in range(addr, end):
        lo, hi = ins[pc]
        k = kind(lo, hi)
        out.append("L%03X: %s" % (pc, body(pc, lo, hi)))
        if k and not hi & 0x40000000:
            if simple(pc + 1):
                lo2, hi2 = ins[pc + 1]
                taken = go(target(pc, lo)) if k == "static" else "return (std::uint32_t)bt & 0x7FF;"
                out.append("    { long bt = t; %s if (bt >= 0) { %s } %s }"
                           % (body(pc + 1, lo2, hi2), taken, go(pc + 2)))
            else:
                out.append("    r.branch = t; return 0x%03Xu;" % ((pc + 1) & 0x7FF))
        elif hi & 0x40000000:
            if k is None and simple(pc + 1):
                lo2, hi2 = ins[pc + 1]
                out.append("    %s r.done = true; return 0x%03Xu;" % (body(pc + 1, lo2, hi2), (pc + 2) & 0x7FF))
            else:
                out.append("    r.branch = t; r.ending = true; return 0x%03Xu;" % ((pc + 1) & 0x7FF))
        elif pc + 1 >= end:
            out.append("    return 0x%03Xu;" % ((pc + 1) & 0x7FF))
    out.append("}")
    return name, count, fnv(words), "\n".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--iso")
    ap.add_argument("--empty", action="store_true")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    chunks = []
    if not args.empty:
        if not args.iso:
            ap.error("--iso or --empty")
        with open(args.iso, "rb") as f:
            read = level_bytes.elf_reader(level_bytes.iso_file(f, "SCUS_971.98"))
        seen = set()
        for addr, words in uploads(read):
            key = (addr, fnv(words), len(words))
            if key not in seen:
                seen.add(key)
                chunks.append(chunk(addr, words))
    lines = ["// Written by tools/vu1_translate.py from the player's disc: the game's VU1 microcode as C++.",
             "// Build output only; never committed.",
             '#include "vu1_runner.h"', "", "namespace port {", "namespace vu1 {", "namespace core {", ""]
    if chunks:
        lines.append("namespace {")
        for _, _, _, code in chunks:
            lines += ["", code]
        lines += ["", "}  // namespace", ""]
        lines.append("const Chunk kTranslated[] = {")
        for name, count, h, _ in chunks:
            lines.append("    {0x%03X, %d, 0x%08Xu, %s}," % (int(name[6:9], 16), count, h, name))
        lines.append("};")
    else:
        lines.append("const Chunk kTranslated[] = {{0, 0, 0, nullptr}};")
    lines.append("const std::size_t kTranslatedCount = %d;" % len(chunks))
    lines += ["", "}  // namespace core", "}  // namespace vu1", "}  // namespace port", ""]
    out = pathlib.Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    text = "\n".join(lines)
    if not out.exists() or out.read_text() != text:
        out.write_text(text)
    print("vu1_translate: %d chunks" % len(chunks), file=sys.stderr)
    for name, count, h, _ in chunks:
        print("  %s: %d instructions" % (name, count), file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
