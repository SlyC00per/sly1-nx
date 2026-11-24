#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# unpsu.py IN.psu OUTDIR: the files of a PS2 .psu save (512-byte entries: mode at 0, length at 4,
# name at 0x40; a file's data after its entry, padded to 1,024 bytes). Names are checked.
import os, struct, sys
d = open(sys.argv[1], 'rb').read()
out = sys.argv[2]
at = 0
while at + 512 <= len(d):
    mode, length = struct.unpack_from('<II', d, at)
    name = d[at + 0x40:at + 0x240].split(b'\0')[0].decode('latin1')
    at += 512
    kind = 'dir' if mode & 0x20 else 'file' if mode & 0x10 else '?'
    print('%-4s mode %04X length %6d %r' % (kind, mode & 0xFFFF, length, name))
    if kind == 'file':
        data = d[at:at + length]
        at += (length + 1023) // 1024 * 1024
        safe = name and all(c.isalnum() or c in '._-!' for c in name) and name not in ('.', '..')
        if safe:
            open(os.path.join(out, name), 'wb').write(data)
        else:
            print('   (name not written)')
print('left over', len(d) - at)
