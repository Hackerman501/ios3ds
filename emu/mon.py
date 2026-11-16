#!/usr/bin/env python3
"""Talk to a running emu/run.py QEMU monitor:  emu/mon.py "info registers" "x/16wx 0x80000000"
Or start a session that boots and stops after N seconds:  emu/mon.py --boot 6 "cmd" ...
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from run import hmp, QEMU, OUT, ROOT  # noqa: E402

ANSI = __import__("re").compile(r"\x1b\[[0-9;]*[A-Za-z]")


def clean(s):
    lines = [ANSI.sub("", l) for l in s.splitlines()]
    return "\n".join(l for l in lines if not l.startswith("(qemu)") and not l.endswith("\x1b[D"))


def main():
    args = sys.argv[1:]
    mon = os.path.join(OUT, "monitor.sock")
    proc = None
    if args and args[0] == "--boot":
        t = float(args[1])
        args = args[2:]
        if os.path.exists(mon):
            os.remove(mon)
        proc = subprocess.Popen([QEMU, "-M", "nintendo3ds", "-accel", "tcg,thread=multi", "-kernel",
                                 os.path.join(ROOT, "build", "boot", "ios3ds.firm"),
                                 "-display", "none", "-serial", "none",
                                 "-monitor", "unix:%s,server,nowait" % mon,
                                 "-drive", "file=%s,if=sd,format=raw" % os.path.join(ROOT, "build", "sd.img")],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(t)
        hmp(mon, "stop")
    for c in args:
        out = hmp(mon, c, timeout=5)
        print("## " + c)
        print(clean(out).split("\n", 1)[-1] if "\n" in out else out)
    if proc:
        hmp(mon, "quit")
        proc.wait(timeout=10)


if __name__ == "__main__":
    main()
