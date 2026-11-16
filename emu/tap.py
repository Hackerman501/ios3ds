#!/usr/bin/env python3
"""Headless touch test: boot, tap iOS points at given times, take screenshots.

  emu/tap.py --time 470 --tap 450:47,320 --shots 448,460,469
  (--tap t:x,y with x,y in iOS points of the 320x480 portrait screen)
Screenshots (upright iOS view) go to build/emu/tap_<t>.png.
"""
import argparse
import os
import subprocess
import tempfile
import time

from PIL import Image

import viewer

ap = argparse.ArgumentParser()
ap.add_argument("--time", type=float, default=470)
ap.add_argument("--tap", action="append", default=[])
ap.add_argument("--shots", default="")
ap.add_argument("--sd", default=os.path.join(viewer.ROOT, "build", "sd.img"))
a = ap.parse_args()
out = os.path.join(viewer.ROOT, "build", "emu")
os.makedirs(out, exist_ok=True)
tmp = tempfile.mkdtemp(prefix="ios3ds-tap-")
sock = os.path.join(tmp, "qmp.sock")
proc = subprocess.Popen([viewer.QEMU, "-M", "nintendo3ds", "-accel", "tcg,thread=multi",
                         "-kernel", os.path.join(viewer.ROOT, "build", "boot", "ios3ds.firm"),
                         "-drive", "file=%s,if=sd,format=raw" % a.sd, "-serial", "none",
                         "-display", "none", "-qmp", "unix:%s,server,nowait" % sock],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
q = viewer.QMP(sock)
ev = [(float(s), "shot", None) for s in filter(None, a.shots.split(","))]
for t in a.tap:
    ts, xy = t.split(":")
    x, y = (float(v) for v in xy.split(","))
    ev += [(float(ts), "down", (x, y)), (float(ts) + 0.15, "up", (x, y))]
ev.append((a.time, "quit", None))
t0 = time.time()
for t, kind, arg in sorted(ev, key=lambda e: e[0]):
    time.sleep(max(0, t - (time.time() - t0)))
    if kind == "shot":
        f = os.path.join(tmp, "s.ppm")
        q.cmd("screendump", filename=f)
        Image.open(f).crop((20, 0, 380, 240)).rotate(-90, expand=True).save(
            os.path.join(out, "tap_%g.png" % t))
    elif kind in ("down", "up"):
        dx, dy = viewer.ios_to_display(arg[0] / 320.0, arg[1] / 480.0)
        q.pointer(dx, dy, kind == "down")
    else:
        q.cmd("quit")
proc.wait(timeout=20)
