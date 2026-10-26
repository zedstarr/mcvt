#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Christian Farrow (zedstarr)
"""MCVT.PIC: TERM.APP's 32x32 icon with "VT100" on the terminal screen.
PIC: 'PIC',0xdc,'00', count, then per bitmap {crc, w, h, bytes, offset32},
rows of 4 bytes, LSB = leftmost pixel; crc = CRC-CCITT (init 0) of the bits."""
import struct, sys
# Usage: mkicon.py TERM.APP   (the icon is taken from your own copy of TERM.APP;
#        a .PIC file holding a 32x32 bitmap works too)
src = sys.argv[1] if len(sys.argv) > 1 else 'TERM.APP'
p = open(src, 'rb').read()
if p[:4] != b'PIC\xdc':                     # an image: icon = first add-file
    off, ln = struct.unpack_from('<HH', p, 0x28)
    p = p[off:off + ln]
crc, w, h, sz, off = struct.unpack_from('<HHHHI', p, 8)
rows = [int.from_bytes(p[20 + y*4:24 + y*4], 'little') for y in range(h)]

def setpx(x, y, on):
    global rows
    rows[y] = rows[y] | (1 << x) if on else rows[y] & ~(1 << x)

# clear a band across the screen (interior is columns 4..27)
for y in range(8, 15):
    for x in range(5, 27): setpx(x, y, 0)
font = {'V': ['#.#', '#.#', '#.#', '#.#', '.#.'],
        'T': ['###', '.#.', '.#.', '.#.', '.#.'],
        '1': ['.#.', '##.', '.#.', '.#.', '###'],
        '0': ['###', '#.#', '#.#', '#.#', '###']}
x0, y0 = 6, 9                      # "VT100" = 5 x 3 + 4 gaps = 19 px, centred
for i, ch in enumerate('VT100'):
    for dy, line in enumerate(font[ch]):
        for dx, c in enumerate(line):
            setpx(x0 + i * 4 + dx, y0 + dy, c == '#')

def crc_ccitt(data):
    c = 0
    for b in data:
        c ^= b << 8
        for _ in range(8): c = ((c << 1) ^ 0x1021) & 0xffff if c & 0x8000 else (c << 1) & 0xffff
    return c
bm = b''.join(r.to_bytes(4, 'little') for r in rows)
out = p[:8] + struct.pack('<HHHHI', crc_ccitt(bm), w, h, sz, off) + bm
open('MCVT.PIC', 'wb').write(out)
for r in rows: print(''.join('#' if r >> x & 1 else '.' for x in range(w)))
