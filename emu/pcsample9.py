#!/usr/bin/env python3
"""Sample the ARM9's PC through QEMU's HMP monitor while emu/run.py is
running, and print a histogram by ARM9 function (build/boot/arm9.elf).
Usage: python3 emu/pcsample9.py [--n 200] [--every 0.05]
"""
import argparse
import bisect
import collections
import os
import re
import socket
import subprocess
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOCK = os.path.join(ROOT, "build", "emu", "monitor.sock")
ELF = os.path.join(ROOT, "build", "boot", "arm9.elf")


def regs(cpu):
    """'cpu N' only holds for the monitor session, so both go on one connection"""
    s = socket.socket(socket.AF_UNIX)
    s.connect(SOCK)
    s.settimeout(5)
    buf = b""
    while b"(qemu)" not in buf:
        buf += s.recv(4096)
    s.sendall(b"cpu %d\ninfo registers\n" % cpu)
    buf = b""
    while b"R15=" not in buf or not buf.rstrip().endswith(b"(qemu)"):
        buf += s.recv(65536)
    s.close()
    return buf.decode("latin1")


ap = argparse.ArgumentParser()
ap.add_argument("--n", type=int, default=200)
ap.add_argument("--every", type=float, default=0.05)
ap.add_argument("--cpu", type=int, default=1)
a = ap.parse_args()

syms = []
nm = subprocess.run(["arm-none-eabi-nm", "-n", ELF], capture_output=True, text=True).stdout
for line in nm.splitlines():
    p = line.split()
    if len(p) == 3 and p[1] in "tTwW":
        syms.append((int(p[0], 16), p[2]))
addrs = [x for x, _ in syms]

hist = collections.Counter()
for _ in range(a.n):
    out = regs(a.cpu)
    m = re.search(r"R15=([0-9a-f]{8})", out)
    if m:
        pc = int(m.group(1), 16)
        i = bisect.bisect_right(addrs, pc) - 1
        hist[syms[i][1] if i >= 0 else "?%08x" % pc] += 1
    time.sleep(a.every)
for name, c in hist.most_common(25):
    print("%5d  %s" % (c, name))
