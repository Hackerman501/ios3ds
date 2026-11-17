#!/usr/bin/env python3
"""Attach to a running QEMU (-s, no -S) after the ARM11 hung, print its
registers and every printable string referenced from its stack (the panic
format string and arguments sit in panic()'s frame), plus Thumb/ARM code
return addresses found there.

Usage: python3 emu/gdb/postmortem.py [--depth 0x800]
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from rsp import RSP  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("--depth", type=lambda s: int(s, 0), default=0x800)
a = ap.parse_args()

g = RSP()
g.select("1")
r = g.regs()
print("pc %08x lr %08x sp %08x cpsr %08x" % (r[15], r[14], r[13], r[16]))
print("r0-r12", " ".join("%08x" % x for x in r[:13]))
try:
    st = [g.u32(0x80840000 + o) for o in (0x40, 0x4C, 0x80, 0x88, 0x8C, 0x98)]
    print("shim init %u timer %u | abs %u cpsr %08x | dec %u | irq-entries %u" % tuple(st))
except IOError:
    print("shim not mapped")


def cstr(va):
    try:
        b = g.read(va, 160).split(b"\0")[0]
    except IOError:
        return None
    s = b.decode("latin1")
    return s if len(s) >= 4 and all(c.isprintable() or c in "\n\t" for c in s) else None


sp = r[13]
for off in range(0, a.depth, 4):
    try:
        w = g.u32(sp + off)
    except IOError:
        break
    note = ""
    if 0x80000000 <= w < 0x88000000:
        s = cstr(w)
        if s:
            note = "str %r" % s
        elif w & 1 and w < 0x80800000:
            note = "thumb code?"
    if note:
        print("  sp+%03x %08x  %s" % (off, w, note))
