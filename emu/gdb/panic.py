#!/usr/bin/env python3
"""Stop at the kernel's _panic and print the format string, its arguments,
the caller and a raw stack dump (the panic text may never reach the console).

Start QEMU with -s -S (plus -icount shift=2,sleep=off to get real-hardware
timing). Usage: python3 emu/gdb/panic.py [--kernel build/sd/ios3ds/kernel.bin]
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from rsp import RSP  # noqa: E402

PANIC = 0x80015C7C        # _panic (Thumb entry 0x80015c7d)

ap = argparse.ArgumentParser()
ap.add_argument("--hits", type=int, default=1)
a = ap.parse_args()

g = RSP()
g.select("1")
g.hbreak(PANIC)


def cstr(va, n=200):
    try:
        b = g.read(va, n)
    except IOError:
        return None
    return b.split(b"\0")[0].decode("latin1", "replace")


for hit in range(a.hits):
    g.cont()
    r = g.regs()
    print("panic #%d: lr=%08x sp=%08x cpsr=%08x" % (hit + 1, r[14], r[13], r[16]))
    print("  fmt   %08x %r" % (r[0], cstr(r[0])))
    for i in (1, 2, 3):
        s = cstr(r[i], 120) if 0x80000000 <= r[i] < 0x88000000 else None
        print("  r%d    %08x %s" % (i, r[i], repr(s) if s and s.isprintable() else ""))
    stk = g.read(r[13], 0x100)
    for o in range(0, 0x100, 16):
        ws = [int.from_bytes(stk[o + j:o + j + 4], "little") for j in range(0, 16, 4)]
        print("  %08x: %s" % (r[13] + o, " ".join("%08x" % w for w in ws)))
