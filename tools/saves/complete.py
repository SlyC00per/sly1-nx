#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# complete.py ELF IN OUT: a Sly 1 save with every entry's done bits set that the executable's table
# gives it (visited, and key 2, vault 4, sprint 8 where the entry has them), the five levels' done
# bit, and the checksum made anew. Everything else is left as the save has it.
import struct, sys
elf = open(sys.argv[1], 'rb').read()
phoff, = struct.unpack_from('<I', elf, 0x1C); phnum, = struct.unpack_from('<H', elf, 0x2C)
def at(addr):
    for k in range(phnum):
        t, off, va, pa, fs, ms = struct.unpack_from('<IIIIII', elf, phoff + k * 32)
        if t == 1 and va <= addr < va + fs: return off + addr - va
T = at(0x00247AF0)
s = bytearray(open(sys.argv[2], 'rb').read())
assert len(s) == 0x1A00 and struct.unpack_from('<ii', s, 0) == (0x12, 0x1A00)
for i in range(46):
    id_, _, flags = struct.unpack_from('<iII', elf, T + i * 0x2C + 0x20)
    if id_ >> 8 == 0: continue
    o = (id_ >> 8) * 0x44C + 0x10 + (id_ & 0xFF) * 0x78
    struct.pack_into('<I', s, o, struct.unpack_from('<I', s, o)[0] | 1 | (flags & 0xE))
for k in range(1, 6):
    o = 0x10 + k * 0x44C + 0x448
    struct.pack_into('<I', s, o, struct.unpack_from('<I', s, o)[0] | 0x21)
struct.pack_into('<i', s, 8, sum(s[0xC:]))
open(sys.argv[3], 'wb').write(s)
