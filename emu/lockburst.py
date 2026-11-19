#!/usr/bin/env python3
"""Lock/unlock burst (hardware run 17): boot iOS in QEMU, put the monitor log
on the top screen (X: screen mode "bottom", the console is then drawn by the
ARM9), then lock and wake many times with POWER taps and shell close/open,
some of them rapid. Afterwards the SD log must show
  - the GPU mirror copying again after every wake (no "screen copy: ARM9 CPU
    again" while the display is on),
  - the ARM9 monitor loop rate staying up ("[power] ... loops/s"),
  - every POWER press and shell event seen by the ARM9 ([mcu] counters),
and the screen must show iOS (not black) at the end.

  python3 emu/lockburst.py --rounds 12
Writes build/emu/lockburst/report.md and screenshots. Exit 1 on failure.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
import time

from PIL import Image, ImageStat

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import viewer  # noqa: E402
from soak import read_log  # noqa: E402

ROOT = viewer.ROOT


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sd", default=os.path.join(ROOT, "build", "sd.img"))
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "emu", "lockburst"))
    ap.add_argument("--rounds", type=int, default=12)
    ap.add_argument("--boot-timeout", type=float, default=1200)
    ap.add_argument("--no-console", action="store_true", help="keep screen mode 'both' (log hidden)")
    ap.add_argument("--then", default="", help="'wallpaper': open Settings > Wallpaper afterwards")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    tmp = tempfile.mkdtemp(prefix="ios3ds-lockburst-")
    sock = os.path.join(tmp, "qmp.sock")
    cmd = [viewer.QEMU, "-M", "nintendo3ds", "-accel", "tcg,thread=multi",
           "-kernel", os.path.join(ROOT, "build", "boot", "ios3ds.firm"),
           "-drive", "file=%s,if=sd,format=raw" % a.sd, "-serial", "none",
           "-display", "none", "-qmp", "unix:%s,server,nowait" % sock]
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    q = viewer.QMP(sock)
    t0 = time.time()
    log = lambda m: print("[burst %6.0fs] %s" % (time.time() - t0, m), flush=True)  # noqa: E731

    def shot(name, full=False):
        f = os.path.join(tmp, "s.ppm")
        q.cmd("screendump", filename=f)
        img = Image.open(f)
        if not full:
            img = img.crop((20, 0, 380, 240)).rotate(-90, expand=True)
        img.save(os.path.join(a.out, name + ".png"))
        return img

    def key(qcode, hold=0.15):
        q.key(qcode, True)
        time.sleep(hold)
        q.key(qcode, False)

    def tap(x, y, hold=0.15):
        dx, dy = viewer.ios_to_display(x / 320.0, y / 480.0)
        q.pointer(dx, dy, True)
        time.sleep(hold)
        q.pointer(dx, dy, False)

    booted = None
    while time.time() - t0 < a.boot_timeout:
        time.sleep(10)
        if "[input] connected to SpringBoard" in read_log(a.sd):
            booted = time.time() - t0
            break
        if proc.poll() is not None:
            break
    ok = booted is not None
    presses = shells = 0
    if ok:
        log("SpringBoard up after %.0f s" % booted)
        time.sleep(40)
        if not a.no_console:
            key("x")                            # both -> bottom: log on the top screen
            time.sleep(2)
        for r in range(a.rounds):
            kind = r % 3
            if kind == 0:                       # POWER: lock, wait, wake
                key("p"); presses += 1
                time.sleep(3)
                key("p"); presses += 1
                time.sleep(3)
            elif kind == 1:                     # shell close / open
                key("c"); shells += 1
                time.sleep(2)
                key("c"); shells += 1
                time.sleep(3)
            else:                               # rapid: four POWER taps 0.4 s apart
                for _ in range(4):
                    key("p", 0.08); presses += 1
                    time.sleep(0.4)
                time.sleep(3)
            log("round %d (%s) done" % (r, ("power", "shell", "rapid power")[kind]))
        # end state: make sure the screen is on (a POWER tap wakes it if not)
        time.sleep(3)
        img = shot("end_ios")
        if ImageStat.Stat(img.convert("L")).mean[0] < 8:
            log("screen dark at the end: one more POWER tap")
            key("p"); presses += 1
            time.sleep(4)
            img = shot("end_ios_after_tap")
        bright = ImageStat.Stat(img.convert("L")).mean[0]
        shot("end_full", full=True)
        if a.then == "wallpaper":
            key("h")                            # HOME: unlock slider / home screen
            time.sleep(3)
            shot("wp0_home")
        time.sleep(5)
    q.cmd("quit")
    proc.wait(timeout=30)
    text = read_log(a.sd)
    with open(os.path.join(a.out, "log.txt"), "w") as f:
        f.write(text)
    lps = [int(m) for m in re.findall(r"\[power\] iOS screen (?:off|on again)[^\n]*?(\d+) loops/s", text)]
    stopped_on = len(re.findall(r"\[gpu\] screen copy: ARM9 CPU again", text))
    resumed = len(re.findall(r"\[gpu\] screen copy: PICA200", text))
    offs = len(re.findall(r"\[power\] iOS screen off", text))
    ons = len(re.findall(r"\[power\] iOS screen on again", text))
    shell_ev = len(re.findall(r"\[mcu\] shell (?:opened|closed)", text))
    report = [
        "# Lock/unlock burst",
        "- boot: %s" % ("%.0f s" % booted if booted else "FAILED"),
        "- POWER taps sent %d, shell events sent %d (seen by the ARM9: %d)" % (presses, shells, shell_ev),
        "- iOS screen off %d times, on again %d times" % (offs, ons),
        "- ARM9 loops/s in [power] lines: %s (min %s)" % (lps, min(lps) if lps else "-"),
        "- GPU screen copy resumed %d times; 'ARM9 CPU again' %d times" % (resumed, stopped_on),
        "- end screen brightness %.1f" % (bright if ok else -1),
    ]
    fail = (not ok or not lps or min(lps) < 20 or stopped_on > 0 or shell_ev < shells or
            offs < 3 or (ok and bright < 8))
    report.append("\n**Result: %s**" % ("FAIL" if fail else "PASS"))
    with open(os.path.join(a.out, "report.md"), "w") as f:
        f.write("\n".join(report) + "\n")
    print("\n".join(report))
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
