#!/usr/bin/env python3
"""Build MCVT.RSC (uncompressed rcomp layout) with MC400 WIMP menu and
dialog resources, whose formats were taken from the ROM's S_.RSC, FLMAN and
TERM.APP. rcomp's own structures are Series 3 ones, so the file is made here.

Layout: word index_offset, word index_bytes, resources..., index of word
offsets (one per resource plus an end sentinel).
"""
import struct, sys

def w(*v): return struct.pack('<%dH' % len(v), *v)
def z(s): return s.encode('latin1') + b'\0'

def menu_item(cmd, hot, flags, text):            # flags 4 plain, 5 "...", |8 separator below
    body = w(cmd, 0) + bytes([ord(hot), flags]) + z(text)
    return w(len(body)) + body

def menu(items):
    return w(len(items)) + b''.join(items)

def bar_entry(rsc, title, flags=0x14, hook=0):    # 0x10: opens pull-down resource rsc
    body = w(hook, rsc) + bytes([0, flags]) + z(title)
    return w(len(body)) + body

def ctl(x, y, f, cid, typ, rest):
    body = w(x, y, f) + bytes([cid, typ]) + rest
    return w(len(body)) + body

def checkbox(cid, x, y, text): return ctl(x, y, 1, cid, 8, w(0x20, len(text) + 2) + z(text))
def button(cid, x, y, text, default=False):
    return ctl(x, y, 0xc00 if default else 0, cid, 7, w(0x20, 10) + z(text))
def label(x, y, text): return ctl(x, y, 0, 0, 2, w(0x20, len(text) + 2) + z(text))

def dialog(wd, ht, flags, title, ctls):
    return w(wd, ht, flags) + z(title) + w(len(ctls)) + b''.join(ctls)

# ---- command numbers = methods ADDed to the command manager class (#66 has 1..8)
C_STOP, C_CLEAR, C_RESET, C_SERIAL, C_TERM = 8, 9, 10, 11, 12

res = [
    z('MCVT'),                                                        # 1 title
    menu([bar_entry(3, 'File'), bar_entry(4, 'Options')]),            # 2 menu bar
    menu([menu_item(C_CLEAR, 'c', 4, 'Clear screen'),                 # 3 File
          menu_item(C_RESET, 'r', 0x0c, 'Reset terminal'),
          menu_item(C_STOP, 'e', 4, 'Exit')]),
    menu([menu_item(C_SERIAL, 'p', 5, 'Serial'),                      # 4 Options
          menu_item(C_TERM, 't', 5, 'Terminal')]),
    dialog(160, 108, 0, 'Terminal options', [                         # 5 dialog
          checkbox(3, 15, 8, 'Local echo'),
          checkbox(4, 15, 22, 'Backspace sends Ctrl-H (not DEL)'),
          checkbox(5, 15, 36, 'Auto wrap at right margin'),
          checkbox(6, 15, 50, 'UTF-8 (else ISO 8859-1)'),
          checkbox(7, 15, 64, 'Send stty to host on resize'),
          button(1, 25, 86, 'CANCEL'),
          button(2, 95, 86, 'ENTER', True)]),
]

data = b''
offs = []
for r in res:
    offs.append(4 + len(data)); data += r
offs.append(4 + len(data))
idx = b''.join(w(o) for o in offs)
out = w(4 + len(data), len(idx)) + data + idx
open(sys.argv[1] if len(sys.argv) > 1 else 'MCVT.RSC', 'wb').write(out)
print('MCVT.RSC', len(out), 'bytes,', len(res), 'resources')
