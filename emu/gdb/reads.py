#!/usr/bin/env python3
"""Log every shim_get_abstime call (clock read) with the shim's probe
counters, to compare QEMU's order of events with the hardware probes.

Start QEMU with -s -S (add -icount shift=2,sleep=off for ~250 MHz
instruction-clocked time, roughly the Old 3DS ARM11). Usage:
  python3 emu/gdb/reads.py [--n 60]
Columns: read#, CPSR at the call, lr, deadline sets, last deadline, IRQ
entries, timer IRQs, private timer count.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from rsp import RSP  # noqa: E402

SHIM = 0x80840000
ABSTIME = SHIM + 0x10

ap = argparse.ArgumentParser()
ap.add_argument("--n", type=int, default=60)
a = ap.parse_args()

g = RSP()
g.select("1")
g.hbreak(ABSTIME)
while True:
    g.cont()
    try:
        st = g.read(SHIM + 0x40, 0x60)
    except IOError:
        continue
    w = [int.from_bytes(st[i:i + 4], "little") for i in range(0, 0x60, 4)]
    inited, timer_irqs, calls = w[0], w[3], w[0x40 // 4]
    dec, dec_last, irq_n = w[0x4C // 4], w[0x50 // 4], w[0x58 // 4]
    if not inited:
        continue
    snap = g.read(SHIM + 0x5F00, 4)
    print("read %3d cpsr %08x lr %08x | dec %u last %u | irq %u timer %u | pt %08x" %
          (calls + 1, g.cpsr(), g.reg(14), dec, dec_last, irq_n, timer_irqs,
           int.from_bytes(snap, "little")), flush=True)
    if calls + 1 >= a.n:
        break
