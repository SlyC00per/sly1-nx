#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# saveinfo.py ELF SAVE...: a Sly 1 save's head, its checksum (the bytes from +0xC summed, ChecksumGameProgress)
# and its completion as ComputeCompletionPercent counts it, from the executable's table of 46 keyed entries.
import struct, sys
elf = open(sys.argv[1], 'rb').read()
phoff, = struct.unpack_from('<I', elf, 0x1C); phnum, = struct.unpack_from('<H', elf, 0x2C)
def at(addr):
    for k in range(phnum):
        t, off, va, pa, fs, ms = struct.unpack_from('<IIIIII', elf, phoff + k * 32)
        if t == 1 and va <= addr < va + fs: return off + addr - va
    raise SystemExit('address not in the file')
T = at(0x00247AF0)
entries = [struct.unpack_from('<iII', elf, T + i * 0x2C + 0x20) for i in range(46)]   # id, name, flags
for path in sys.argv[2:]:
    s = open(path, 'rb').read()
    typ, size, total = struct.unpack_from('<iii', s, 0)
    time, = struct.unpack_from('<f', s, 0xC)
    good = sum(s[0xC:]) == total
    done = all_ = 0
    for id_, _, flags in entries:
        lv = id_ >> 8
        if lv == 0: continue
        w, = struct.unpack_from('<I', s, lv * 0x44C + 0x10 + (id_ & 0xFF) * 0x78)
        all_ += 1; done += w & 1
        for bit in (2, 4, 8):
            if flags & bit:
                all_ += 1; done += 1 if w & bit else 0
    for k in range(5):
        all_ += 1
        done += 1 if struct.unpack_from('<I', s, 0x8A4 + k * 0x44C)[0] & 0x20 else 0
    lvl, ent, lives, spare, coins, opt, f0, f4, mov, hint = struct.unpack_from('<iiiiiIIIIi', s, 0x19D8)
    print('%s: %d bytes, type 0x%X, size 0x%X, checksum %s, played %.0f s (%.1f h); done %d of %d = %d%%; at level %d entry %d, lives %d, coins %d, options 0x%X, flags 0x%08X 0x%08X movies 0x%X'
          % (path.split('/')[-1], len(s), typ, size, 'good' if good else 'BAD', time, time / 3600, done, all_, 100 * done // all_ if all_ else 0, lvl, ent, lives, coins, opt, f0, f4, mov))
