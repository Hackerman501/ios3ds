#!/usr/bin/env python3
"""Interactive QEMU session for step-by-step UI tests (one command per call).

  emu/session.py start [--wifi] [--icount 2] [--extra "..."]   boot in the background
  emu/session.py tap X Y            tap iOS point (320x480 portrait)
  emu/session.py drag X0 Y0 X1 Y1 [SECONDS]
  emu/session.py type "text"        tap the iOS keyboard keys for the text (lower case,
                                    digits and -/:;()$&@" via the 123 page, which stays up
                                    until a letter needs the ABC page; the page is
                                    remembered between calls, --page abc|123 overrides)
  emu/session.py key QCODE [HOLD]   3DS button (h = HOME, ...)
  emu/session.py shot NAME          upright iOS screenshot -> <dir>/NAME.png
  emu/session.py log [PATTERN]      grep the live sd:/ios3ds/log.txt and the QEMU output
  emu/session.py stop

--wifi adds the ctr-wifi model's access point on slirp (docs/wifi.md section 7).
State lives in build/emu-session/ (the SD image must not be used by another QEMU).
"""
import argparse
import json
import os
import re
import signal
import subprocess
import sys
import tempfile
import time

from PIL import Image

import viewer

DIR = os.path.join(viewer.ROOT, "build", "emu-session")
SOCK = os.path.join(DIR, "qmp.sock")

# iOS 4 English keyboard, portrait (points): rows of letters and their y
ROWS = [("qwertyuiop", 0, 32, 291), ("asdfghjkl", 14, 32, 347), ("zxcvbnm", 44, 32, 400)]
NUM_ROWS = [("1234567890", 0, 32, 291), ("-/:;()$&@\"", 0, 32, 347)]
MODE_KEY = (22, 457)        # "123" / "ABC"


def key_xy(ch):
    for row, x0, w, y in ROWS:
        if ch in row:
            return "abc", (x0 + w * row.index(ch) + w / 2, y)
    for row, x0, w, y in NUM_ROWS:
        if ch in row:
            return "123", (x0 + w * row.index(ch) + w / 2, y)
    if ch == " ":
        return None, (160, 457)
    raise SystemExit("session: no key for %r" % ch)


def qmp():
    return viewer.QMP(SOCK)


def tap(q, x, y, hold=0.15):
    dx, dy = viewer.ios_to_display(x / 320.0, y / 480.0)
    q.pointer(dx, dy, True)
    time.sleep(hold)
    q.pointer(dx, dy, False)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd")
    ap.add_argument("args", nargs="*")
    ap.add_argument("--wifi", action="store_true")
    ap.add_argument("--icount", default="")
    ap.add_argument("--extra", default="")
    ap.add_argument("--sd", default=os.path.join(viewer.ROOT, "build", "sd.img"))
    ap.add_argument("--page", choices=("abc", "123"), help="keyboard page before 'type'")
    a = ap.parse_args()
    os.makedirs(DIR, exist_ok=True)
    if a.cmd == "start":
        for f in (SOCK, os.path.join(DIR, "kbpage")):
            if os.path.exists(f):
                os.remove(f)
        cmd = [viewer.QEMU, "-M", ("nintendo3ds,new3ds=on" if os.environ.get("IOS3DS_NEW3DS") == "1" else "nintendo3ds"), "-accel",
               "tcg,thread=%s" % ("single" if a.icount else "multi"),
               "-kernel", os.path.join(viewer.ROOT, "build", "boot", "ios3ds.firm"),
               "-drive", "file=%s,if=sd,format=raw" % a.sd, "-serial", "none",
               "-display", "none", "-qmp", "unix:%s,server,nowait" % SOCK]
        if a.icount:
            cmd += ["-icount", "shift=%s,sleep=off" % a.icount, "-rtc", "clock=vm"]
        if a.wifi:
            cmd += ["-nic", "user,model=ctr-wifi"]
        cmd += a.extra.split()
        log = open(os.path.join(DIR, "qemu.txt"), "w")
        p = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        with open(os.path.join(DIR, "pid"), "w") as f:
            f.write(str(p.pid))
        with open(os.path.join(DIR, "t0"), "w") as f:
            f.write(str(time.time()))
        print("[session] started pid %d" % p.pid)
        return
    if a.cmd == "stop":
        try:
            qmp().cmd("quit")
        except Exception:
            pid = int(open(os.path.join(DIR, "pid")).read())
            os.kill(pid, signal.SIGTERM)
        print("[session] stopped")
        return
    if a.cmd == "log":
        sd = a.sd
        r = subprocess.run(["mtype", "-i", "%s@@%d" % (sd, 2048 * 512), "::/ios3ds/log.txt"],
                           capture_output=True)
        text = r.stdout.decode(errors="replace") + open(os.path.join(DIR, "qemu.txt")).read()
        pat = re.compile(a.args[0]) if a.args else None
        for line in text.splitlines():
            if not pat or pat.search(line):
                print(line)
        return
    q = qmp()
    t = time.time() - float(open(os.path.join(DIR, "t0")).read())
    if a.cmd == "tap":
        tap(q, float(a.args[0]), float(a.args[1]))
    elif a.cmd == "drag":
        x0, y0, x1, y1 = (float(v) for v in a.args[:4])
        secs = float(a.args[4]) if len(a.args) > 4 else 0.4
        n = max(10, int(secs * 20))
        dx, dy = viewer.ios_to_display(x0 / 320.0, y0 / 480.0)
        q.pointer(dx, dy, True)
        for i in range(1, n + 1):
            f = i / float(n)
            dx, dy = viewer.ios_to_display((x0 + (x1 - x0) * f) / 320.0, (y0 + (y1 - y0) * f) / 480.0)
            time.sleep(secs / n)
            q.pointer(dx, dy, None)
        time.sleep(0.05)
        q.pointer(dx, dy, False)
    elif a.cmd == "type":
        pfile = os.path.join(DIR, "kbpage")
        page = a.page or (open(pfile).read().strip() if os.path.exists(pfile) else "abc")
        for ch in a.args[0]:
            want, xy = key_xy(ch)
            if want and want != page:
                tap(q, *MODE_KEY)
                time.sleep(0.6)
                page = want
            tap(q, *xy, hold=0.12)
            time.sleep(0.5)
        with open(pfile, "w") as f:         # iOS 4 stays on 123 after - (seen in QEMU)
            f.write(page)
    elif a.cmd == "key":
        hold = float(a.args[1]) if len(a.args) > 1 else 0.2
        q.key(a.args[0], True)
        time.sleep(hold)
        q.key(a.args[0], False)
    elif a.cmd == "shot":
        tmpd = tempfile.mkdtemp(prefix="ios3ds-shot-")
        f = os.path.join(tmpd, "s.ppm")
        q.cmd("screendump", filename=f)
        time.sleep(0.3)
        im = Image.open(f)
        out = os.path.join(DIR, a.args[0] + ".png")
        im.crop((20, 0, 380, 240)).rotate(-90, expand=True).save(out)
        print("[session] %s (t=%.0f s)" % (out, t))
        return
    else:
        raise SystemExit("session: unknown command " + a.cmd)
    print("[session] %s done (t=%.0f s)" % (a.cmd, t))


if __name__ == "__main__":
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    main()
