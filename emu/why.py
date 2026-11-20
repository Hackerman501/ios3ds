#!/usr/bin/env python3
"""Boot in the emulator and explain the first CPU exception XNU takes.

  emu/why.py [--time 10] [--skip N]

Prints the exception type, fault address (DFAR) + its physical translation,
the last translated code blocks before it (guest PCs), and the tail of the
XNU log ring as seen in sd:/ios3ds/log.txt.
"""
import argparse
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from run import hmp, QEMU, OUT, ROOT  # noqa: E402
from mon import clean  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--time", type=float, default=10)
    ap.add_argument("--skip", type=int, default=0, help="ignore the first N exceptions after the jump")
    ap.add_argument("--blocks", type=int, default=6)
    a = ap.parse_args()
    os.makedirs(OUT, exist_ok=True)
    mon = os.path.join(OUT, "monitor.sock")
    log = os.path.join(OUT, "why.log")
    if os.path.exists(mon):
        os.remove(mon)
    p = subprocess.Popen([QEMU, "-M", ("nintendo3ds,new3ds=on" if os.environ.get("IOS3DS_NEW3DS") == "1" else "nintendo3ds"), "-accel", "tcg,thread=multi", "-kernel", os.path.join(ROOT, "build", "boot", "ios3ds.firm"),
                          "-display", "none", "-serial", "none", "-monitor", "unix:%s,server,nowait" % mon,
                          "-drive", "file=%s,if=sd,format=raw" % os.path.join(ROOT, "build", "sd.img"),
                          "-d", "int,in_asm,nochain", "-D", log],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(a.time)
    hmp(mon, "stop")
    regs = clean(hmp(mon, "info registers"))
    lines = open(log, errors="replace").read().splitlines()
    # only exceptions taken by XNU (PC >= 0x80000000 region or abort with DFAR)
    idx = [i for i, l in enumerate(lines) if l.startswith("Taking exception")]
    xnu = []
    for i in idx:
        blk = [l for l in lines[max(0, i - 400):i] if l.startswith("0x")]
        last = blk[-1] if blk else ""
        if last.startswith("0x8") or last.startswith("0xc") or last.startswith("0xf"):
            xnu.append(i)
    if len(xnu) <= a.skip:
        print("no XNU exception found (%d total)" % len(idx))
    else:
        i = xnu[a.skip]
        print("\n".join(lines[i:i + 4]))
        m = re.search(r"DFAR 0x([0-9a-f]+)", "\n".join(lines[i:i + 4]))
        if m:
            print("DFAR -> " + clean(hmp(mon, "gva2gpa 0x" + m.group(1))).strip().splitlines()[-1])
        pcs = [l.split(":")[0] for l in lines[max(0, i - 3000):i] if l.startswith("0x")]
        print("last code blocks before it:", " ".join(pcs[-a.blocks:]))
    print("current CPU state:\n" + "\n".join(l for l in regs.splitlines() if l.startswith(("R", "PSR"))))
    hmp(mon, "quit")
    p.wait(timeout=10)
    r = subprocess.run(["mtype", "-i", "%s@@%d" % (os.path.join(ROOT, "build", "sd.img"), 2048 * 512),
                        "::/ios3ds/log.txt"], capture_output=True)
    tail = r.stdout.decode(errors="replace").split("[a11] jumping")[-1].strip().splitlines()
    print("--- xnu log (tail) ---")
    print("\n".join(tail[-15:]))


if __name__ == "__main__":
    main()
