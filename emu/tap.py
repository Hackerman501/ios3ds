#!/usr/bin/env python3
"""Headless touch test: boot, tap iOS points at given times, take screenshots.

  emu/tap.py --time 470 --tap 450:47,320 --shots 448,460,469
  (--tap t:x,y with x,y in iOS points of the 320x480 portrait screen)
  emu/tap.py --drag 440:280,240:40,240   swipe from (280,240) to (40,240) in 0.4 s
  (--drag t:x0,y0:x1,y1[:seconds], 10 steps)
  emu/tap.py --key 300:h --key 320:p:5 --set 280:mcu-battery=50 --get 330:lcd-backlight
  (--key t:qcode[:hold seconds, default 0.2]; --set / --get: QOM properties of
   /machine, e.g. the MCU model's, docs/mcu.md)
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
ap.add_argument("--drag", action="append", default=[])
ap.add_argument("--shots", default="")
ap.add_argument("--key", action="append", default=[])
ap.add_argument("--set", action="append", default=[])
ap.add_argument("--get", action="append", default=[])
ap.add_argument("--hmp", action="append", default=[], help="t:monitor command, output printed")
ap.add_argument("--icount", default="", help="as emu/run.py --icount (2 = ~250 MHz, hardware ordering)")
ap.add_argument("--extra", default="", help="more QEMU arguments, e.g. '-name x,debug-threads=on'")
ap.add_argument("--sd", default=os.path.join(viewer.ROOT, "build", "sd.img"))
ap.add_argument("--out", default=os.path.join(viewer.ROOT, "build", "emu"))
a = ap.parse_args()
out = a.out
os.makedirs(out, exist_ok=True)
tmp = tempfile.mkdtemp(prefix="ios3ds-tap-")
sock = os.path.join(tmp, "qmp.sock")
cmd = [viewer.QEMU, "-M", "nintendo3ds", "-accel", "tcg,thread=%s" % ("single" if a.icount else "multi"),
       "-kernel", os.path.join(viewer.ROOT, "build", "boot", "ios3ds.firm"),
       "-drive", "file=%s,if=sd,format=raw" % a.sd, "-serial", "none",
       "-display", "none", "-qmp", "unix:%s,server,nowait" % sock]
if a.icount:   # guest time = instruction count; the RTC follows it too
    cmd += ["-icount", "shift=%s,sleep=off" % a.icount, "-rtc", "clock=vm"]
cmd += a.extra.split()
proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
q = viewer.QMP(sock)
ev = [(float(s), "shot", None) for s in filter(None, a.shots.split(","))]
for t in a.tap:
    ts, xy = t.split(":")
    x, y = (float(v) for v in xy.split(","))
    ev += [(float(ts), "down", (x, y)), (float(ts) + 0.15, "up", (x, y))]
for d in a.drag:
    ts, p0, p1, *dur = d.split(":")
    (x0, y0), (x1, y1) = ([float(v) for v in p.split(",")] for p in (p0, p1))
    secs = float(dur[0]) if dur else 0.4
    n = max(10, int(secs * 20))                 # a move every 50 ms or less
    t, step = float(ts), secs / n
    ev.append((t, "down", (x0, y0)))
    for i in range(1, n + 1):
        f = i / float(n)
        ev.append((t + step * i, "move", (x0 + (x1 - x0) * f, y0 + (y1 - y0) * f)))
    ev.append((t + step * n + 0.05, "up", (x1, y1)))
for k in a.key:
    ts, qc, *hold = k.split(":")
    ev += [(float(ts), "kdown", qc), (float(ts) + (float(hold[0]) if hold else 0.2), "kup", qc)]
for st in a.set:
    ts, kv = st.split(":", 1)
    prop, val = kv.split("=", 1)
    val = {"on": True, "true": True, "off": False, "false": False}.get(val, val)
    ev.append((float(ts), "set", (prop, int(val) if isinstance(val, str) else val)))
for g in a.get:
    ts, prop = g.split(":", 1)
    ev.append((float(ts), "get", prop))
for h in a.hmp:
    ts, c = h.split(":", 1)
    ev.append((float(ts), "hmp", c))
ev.append((a.time, "quit", None))
t0 = time.time()
for t, kind, arg in sorted(ev, key=lambda e: e[0]):
    time.sleep(max(0, t - (time.time() - t0)))
    if proc.poll() is not None:            # e.g. iOS shut down and the MCU model powered off
        print("[tap] QEMU exited (status %s) before %gs" % (proc.returncode, t), flush=True)
        break
    if kind == "shot":
        f = os.path.join(tmp, "s.ppm")
        q.cmd("screendump", filename=f)
        im = Image.open(f)
        im.crop((20, 0, 380, 240)).rotate(-90, expand=True).save(os.path.join(out, "tap_%g.png" % t))
        im.save(os.path.join(out, "tap_%g_3ds.png" % t))
    elif kind in ("kdown", "kup"):
        q.key(arg, kind == "kdown")
    elif kind == "set":
        print("[tap] %gs qom-set %s=%s -> %s" % (t, arg[0], arg[1],
              q.cmd("qom-set", path="/machine", property=arg[0], value=arg[1])), flush=True)
    elif kind == "get":
        print("[tap] %gs qom-get %s -> %s" % (t, arg, q.cmd("qom-get", path="/machine", property=arg)),
              flush=True)
    elif kind == "hmp":
        r = q.cmd("human-monitor-command", **{"command-line": arg, "cpu-index": 0})   # 0 = ARM11
        print("[tap] %gs %s ->\n%s" % (t, arg, r.get("return", r)), flush=True)
    elif kind in ("down", "up", "move"):
        dx, dy = viewer.ios_to_display(arg[0] / 320.0, arg[1] / 480.0)
        q.pointer(dx, dy, None if kind == "move" else kind == "down")
    else:
        q.cmd("quit")
proc.wait(timeout=20)
print("[tap] QEMU exit status %s" % proc.returncode)
