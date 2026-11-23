#!/usr/bin/env python3
"""Show and cancel the iOS power-off slider twice; fail on a SpringBoard restart.

  python3 emu/slider_test.py --icount 2

Boots build/sd.img in our QEMU machine, wakes/unlocks the device, holds POWER
long enough for the slider, taps Cancel, repeats, and checks sd:/ios3ds/log.txt
for panics and for a second "[input] connected to SpringBoard" line (the first
one is the boot connection, re-log 63). Screenshots + report go to
build/emu-slider/. Needs a fresh image if combined with other QEMU runs.
"""
import argparse
import atexit
import os
import re
import subprocess
import sys
import tempfile
import time

from PIL import Image, ImageStat

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import viewer  # noqa: E402

ROOT = viewer.ROOT


def read_log(sd):
    r = subprocess.run(["mtype", "-i", "%s@@%d" % (sd, 2048 * 512), "::/ios3ds/log.txt"],
                       capture_output=True, text=True, errors="replace")
    return r.stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sd", default=os.path.join(ROOT, "build", "sd.img"))
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "emu-slider"))
    ap.add_argument("--icount", default="2")
    ap.add_argument("--boot-timeout", type=float, default=600)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    tmp = tempfile.mkdtemp(prefix="ios3ds-slider-")
    sock = os.path.join(tmp, "qmp.sock")
    cmd = [viewer.QEMU, "-M", "nintendo3ds", "-accel",
           "tcg,thread=%s" % ("single" if a.icount else "multi"),
           "-kernel", os.path.join(ROOT, "build", "boot", "ios3ds.firm"),
           "-drive", "file=%s,if=sd,format=raw" % a.sd, "-serial", "none",
           "-display", "none", "-qmp", "unix:%s,server,nowait" % sock]
    if a.icount:
        cmd += ["-icount", "shift=%s,sleep=off" % a.icount, "-rtc", "clock=vm"]
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    atexit.register(lambda: proc.poll() is None and proc.terminate())
    q = viewer.QMP(sock)
    t0 = time.time()

    def shot(name):
        f = os.path.join(tmp, "s.ppm")
        q.cmd("screendump", filename=f)
        img = Image.open(f).crop((20, 0, 380, 240)).rotate(-90, expand=True)
        img.save(os.path.join(a.out, name + ".png"))
        return img

    def tap(x, y, hold=0.15):
        dx, dy = viewer.ios_to_display(x / 320.0, y / 480.0)
        q.pointer(dx, dy, True)
        time.sleep(hold)
        q.pointer(dx, dy, False)

    def key(code, hold):
        q.key(code, True)
        time.sleep(hold)
        q.key(code, False)

    def drag(x0, y0, x1, y1, secs=0.8):
        dx, dy = viewer.ios_to_display(x0 / 320.0, y0 / 480.0)
        q.pointer(dx, dy, True)
        time.sleep(0.15)
        for i in range(1, 11):
            time.sleep(secs / 10)
            f = i / 10.0
            dx, dy = viewer.ios_to_display((x0 + (x1 - x0) * f) / 320.0,
                                           (y0 + (y1 - y0) * f) / 480.0)
            q.pointer(dx, dy, None)
        time.sleep(0.05)
        q.pointer(dx, dy, False)

    booted = False
    while time.time() - t0 < a.boot_timeout:
        time.sleep(10)
        if "[input] connected to SpringBoard" in read_log(a.sd):
            booted = True
            break
        if proc.poll() is not None:
            break
    rounds = []
    if booted:
        # wake and unlock; retry a slide swallowed by the input poll (re-log 75)
        key("ret", 0.2)
        time.sleep(3)
        for _ in range(3):
            drag(40, 420, 300, 420)
            time.sleep(5)
            if ImageStat.Stat(shot("unlock").convert("L")).mean[0] > 10:
                break
        for i in range(2):
            key("p", 5)                     # POWER hold: HELD IRQ after 3 s
            time.sleep(2)
            key("ret", 0.2)                 # wake the screen; leave the slider up
            time.sleep(5)
            bright = ImageStat.Stat(shot("slider%d" % i).convert("L")).mean[0]
            tap(160, 470)                   # Cancel
            time.sleep(4)
            after = ImageStat.Stat(shot("cancel%d" % i).convert("L")).mean[0]
            rounds.append({"round": i, "slider_brightness": round(bright, 2),
                           "after_cancel_brightness": round(after, 2)})
            print("[slider] round %d: slider brightness %.1f, after cancel %.1f" %
                  (i, bright, after), flush=True)
    try:
        q.cmd("quit")
        proc.wait(timeout=30)
    except Exception:
        proc.kill()
    text = read_log(a.sd)
    with open(os.path.join(a.out, "log.txt"), "w") as f:
        f.write(text)
    connects = text.count("[input] connected to SpringBoard")
    bad = [l for l in text.splitlines() if re.search(
        r"panic\(cpu|Fatal Exception|\[probe\] ARM11 stalled|exited abnormally|Segmentation fault|Abort trap", l)]
    failed = not booted or connects > 1 or bool(bad)
    report = {"booted": booted, "rounds": rounds, "springboard_restarts": max(0, connects - 1),
              "errors": bad[:10], "failed": failed}
    import json
    with open(os.path.join(a.out, "report.json"), "w") as f:
        json.dump(report, f, indent=1)
    print("boot: %s; rounds: %d; SpringBoard restarts: %d; errors: %d; Result: %s" %
          (booted, len(rounds), report["springboard_restarts"], len(bad),
           "FAIL" if failed else "PASS"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
