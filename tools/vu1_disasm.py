#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""A listing of the game's VU1 microcode, for writing the renderer's native handlers.

Reads the microcode from the player's disc (as tools/vu1_translate.py does) and prints each upload
as upper and lower operations with their operands. The listing is the game's data: write it to a
build folder only, never commit it.

    python3 tools/vu1_disasm.py --iso DISC.iso [--chunk 300] > build-host/vu1.txt
"""
import argparse
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import level_bytes  # noqa: E402
import vu1_translate  # noqa: E402

BC = "xyzw"


def dest(w):
    d = (w >> 21) & 0xF
    return "".join(c for c, bit in zip("xyzw", (8, 4, 2, 1)) if d & bit)


def special(w):
    return (w & 3) | ((w >> 4) & 0x7C)


def s11(w):
    v = w & 0x7FF
    return v - 0x800 if v & 0x400 else v


def upper(w):
    op, ft, fs, fd = w & 0x3F, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
    d = dest(w)
    if op < 0x1C:
        name = ["ADD", "SUB", "MADD", "MSUB", "MAX", "MINI", "MUL"][op >> 2] + BC[op & 3]
        return "%s.%s vf%02d, vf%02d, vf%02d%s" % (name, d, fd, fs, ft, BC[op & 3])
    names = {0x1C: ("MULq", "q"), 0x1D: ("MAXi", "i"), 0x1E: ("MULi", "i"), 0x1F: ("MINIi", "i"), 0x20: ("ADDq", "q"),
             0x21: ("MADDq", "q"), 0x22: ("ADDi", "i"), 0x23: ("MADDi", "i"), 0x24: ("SUBq", "q"), 0x25: ("MSUBq", "q"),
             0x26: ("SUBi", "i"), 0x27: ("MSUBi", "i")}
    if op in names:
        n, src = names[op]
        return "%s.%s vf%02d, vf%02d, %s" % (n, d, fd, fs, src)
    full = {0x28: "ADD", 0x29: "MADD", 0x2A: "MUL", 0x2B: "MAX", 0x2C: "SUB", 0x2D: "MSUB", 0x2E: "OPMSUB", 0x2F: "MINI"}
    if op in full:
        return "%s.%s vf%02d, vf%02d, vf%02d" % (full[op], d, fd, fs, ft)
    o2 = special(w)
    if o2 < 0x10 or 0x18 <= o2 <= 0x1B:
        k = ["ADDA", "SUBA", "MADDA", "MSUBA"][o2 >> 2] if o2 < 0x10 else "MULA"
        return "%s%s.%s ACC, vf%02d, vf%02d%s" % (k, BC[o2 & 3], d, fs, ft, BC[o2 & 3])
    if 0x10 <= o2 <= 0x17:
        k = ["ITOF0", "ITOF4", "ITOF12", "ITOF15", "FTOI0", "FTOI4", "FTOI12", "FTOI15"][o2 - 0x10]
        return "%s.%s vf%02d, vf%02d" % (k, d, ft, fs)
    acc = {0x1C: "MULAq ACC, vf%02d, q", 0x1E: "MULAi ACC, vf%02d, i", 0x20: "ADDAq ACC, vf%02d, q",
           0x21: "MADDAq ACC, vf%02d, q", 0x22: "ADDAi ACC, vf%02d, i", 0x23: "MADDAi ACC, vf%02d, i",
           0x24: "SUBAq ACC, vf%02d, q", 0x25: "MSUBAq ACC, vf%02d, q", 0x26: "SUBAi ACC, vf%02d, i",
           0x27: "MSUBAi ACC, vf%02d, i"}
    if o2 in acc:
        return (acc[o2] % fs).replace(" ACC", "." + d + " ACC", 1)
    two = {0x28: "ADDA", 0x29: "MADDA", 0x2A: "MULA", 0x2C: "SUBA", 0x2D: "MSUBA", 0x2E: "OPMULA"}
    if o2 in two:
        return "%s.%s ACC, vf%02d, vf%02d" % (two[o2], d, fs, ft)
    if o2 == 0x1D:
        return "ABS.%s vf%02d, vf%02d" % (d, ft, fs)
    if o2 == 0x1F:
        return "CLIP vf%02d.xyz, vf%02dw" % (fs, ft)
    if o2 == 0x2F:
        return "NOP"
    return "U?%08X" % w


def lower(w, pc):
    op, it, is_, id_ = (w >> 25) & 0x7F, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
    d = dest(w)
    fsf, ftf = BC[(w >> 21) & 3], BC[(w >> 23) & 3]
    imm15 = ((w >> 10) & 0x7800) | (w & 0x7FF)
    imm12 = ((w >> 10) & 0x800) | (w & 0x7FF)
    tgt = "0x%03X" % ((pc + 1 + s11(w)) & 0x7FF)
    simple = {0x00: "LQ.%s vf%02d, %d(vi%02d)" % (d, it, s11(w), is_), 0x01: "SQ.%s vf%02d, %d(vi%02d)" % (d, is_, s11(w), it),
              0x04: "ILW.%s vi%02d, %d(vi%02d)" % (d, it, s11(w), is_), 0x05: "ISW.%s vi%02d, %d(vi%02d)" % (d, it, s11(w), is_),
              0x08: "IADDIU vi%02d, vi%02d, %d" % (it, is_, imm15), 0x09: "ISUBIU vi%02d, vi%02d, %d" % (it, is_, imm15),
              0x10: "FCEQ vi01, 0x%06X" % (w & 0xFFFFFF), 0x11: "FCSET 0x%06X" % (w & 0xFFFFFF),
              0x12: "FCAND vi01, 0x%06X" % (w & 0xFFFFFF), 0x13: "FCOR vi01, 0x%06X" % (w & 0xFFFFFF),
              0x14: "FSEQ vi%02d, 0x%03X" % (it, imm12), 0x15: "FSSET 0x%03X" % imm12, 0x16: "FSAND vi%02d, 0x%03X" % (it, imm12),
              0x17: "FSOR vi%02d, 0x%03X" % (it, imm12), 0x18: "FMEQ vi%02d, vi%02d" % (it, is_), 0x1A: "FMAND vi%02d, vi%02d" % (it, is_),
              0x1B: "FMOR vi%02d, vi%02d" % (it, is_), 0x1C: "FCGET vi%02d" % it, 0x20: "B %s" % tgt,
              0x21: "BAL vi%02d, %s" % (it, tgt), 0x24: "JR vi%02d" % is_, 0x25: "JALR vi%02d, vi%02d" % (it, is_),
              0x28: "IBEQ vi%02d, vi%02d, %s" % (it, is_, tgt), 0x29: "IBNE vi%02d, vi%02d, %s" % (it, is_, tgt),
              0x2C: "IBLTZ vi%02d, %s" % (is_, tgt), 0x2D: "IBGTZ vi%02d, %s" % (is_, tgt), 0x2E: "IBLEZ vi%02d, %s" % (is_, tgt),
              0x2F: "IBGEZ vi%02d, %s" % (is_, tgt)}
    if op in simple:
        return simple[op]
    if op != 0x40:
        return "L?%08X" % w
    f = w & 0x3F
    if f < 0x3C:
        imm5 = (w >> 6) & 31
        imm5 = imm5 - 32 if imm5 & 16 else imm5
        return {0x30: "IADD vi%02d, vi%02d, vi%02d" % (id_, is_, it), 0x31: "ISUB vi%02d, vi%02d, vi%02d" % (id_, is_, it),
                0x32: "IADDI vi%02d, vi%02d, %d" % (it, is_, imm5), 0x34: "IAND vi%02d, vi%02d, vi%02d" % (id_, is_, it),
                0x35: "IOR vi%02d, vi%02d, vi%02d" % (id_, is_, it)}.get(f, "L?%08X" % w)
    o2 = special(w)
    table = {0x30: "MOVE.%s vf%02d, vf%02d" % (d, it, is_), 0x31: "MR32.%s vf%02d, vf%02d" % (d, it, is_),
             0x34: "LQI.%s vf%02d, (vi%02d++)" % (d, it, is_), 0x35: "SQI.%s vf%02d, (vi%02d++)" % (d, is_, it),
             0x36: "LQD.%s vf%02d, (--vi%02d)" % (d, it, is_), 0x37: "SQD.%s vf%02d, (--vi%02d)" % (d, is_, it),
             0x38: "DIV Q, vf%02d%s, vf%02d%s" % (is_, fsf, it, ftf), 0x39: "SQRT Q, vf%02d%s" % (it, ftf),
             0x3A: "RSQRT Q, vf%02d%s, vf%02d%s" % (is_, fsf, it, ftf), 0x3B: "WAITQ",
             0x3C: "MTIR vi%02d, vf%02d%s" % (it, is_, fsf), 0x3D: "MFIR.%s vf%02d, vi%02d" % (d, it, is_),
             0x3E: "ILWR.%s vi%02d, (vi%02d)" % (d, it, is_), 0x3F: "ISWR.%s vi%02d, (vi%02d)" % (d, it, is_),
             0x64: "MFP.%s vf%02d, P" % (d, it), 0x68: "XTOP vi%02d" % it, 0x69: "XITOP vi%02d" % it,
             0x6C: "XGKICK vi%02d" % is_, 0x70: "ESADD P, vf%02d" % is_, 0x71: "ERSADD P, vf%02d" % is_,
             0x72: "ELENG P, vf%02d" % is_, 0x73: "ERLENG P, vf%02d" % is_, 0x76: "ESUM P, vf%02d" % is_,
             0x78: "ESQRT P, vf%02d%s" % (is_, fsf), 0x79: "ERSQRT P, vf%02d%s" % (is_, fsf), 0x7A: "ERCPR P, vf%02d%s" % (is_, fsf),
             0x7B: "WAITP", 0x7C: "ESIN P, vf%02d%s" % (is_, fsf), 0x7D: "EATAN P, vf%02d%s" % (is_, fsf),
             0x7E: "EEXP P, vf%02d%s" % (is_, fsf)}
    if o2 == 0x30 and it == 0 and is_ == 0:
        return "NOP"
    return table.get(o2, "L?%08X" % w)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--iso", required=True)
    ap.add_argument("--chunk", help="only the upload at this address (hex)")
    args = ap.parse_args()
    with open(args.iso, "rb") as f:
        read = level_bytes.elf_reader(level_bytes.iso_file(f, "SCUS_971.98"))
    seen = set()
    for addr, words in vu1_translate.uploads(read):
        h = vu1_translate.fnv(words)
        if (addr, h) in seen or (args.chunk and addr != int(args.chunk, 16)):
            continue
        seen.add((addr, h))
        print("; upload at 0x%03X, %d instructions, hash %08X" % (addr, len(words) // 2, h))
        for k in range(len(words) // 2):
            lo, hi = words[2 * k], words[2 * k + 1]
            pc = addr + k
            flags = "".join(c for c, bit in (("E", 30), ("M", 29), ("D", 28), ("T", 27)) if (hi >> bit) & 1)
            lo_text = "LOI 0x%08X" % lo if hi >> 31 else lower(lo, pc)
            print("%03X: %-40s %-36s %s" % (pc, upper(hi), lo_text, flags))
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
