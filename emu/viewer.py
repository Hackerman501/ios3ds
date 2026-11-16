#!/usr/bin/env python3
"""Interactive window for the nintendo3ds QEMU machine (our QEMU build has no
GTK/SDL/VNC display). Starts QEMU headless with a QMP socket, shows the 3DS
screens and an upright view of iOS (the top screen, rotated back), and sends
real key down/up events through QMP.

  emu/viewer.py [--sd build/sd.img] [--firm build/boot/ios3ds.firm] [--fps 8]

Keys (3DS button):  Enter / H = START (iOS Home)   Backspace / K = SELECT (iOS lock)
                    A = A   S = B   X = X   Y = Y   Q = L (vol -)   W = R (vol +)
                    arrow keys = D-pad   (SELECT+START together = XNU panic dump)
Close the window to quit QEMU; the SD log stays in sd:/ios3ds/log.txt.
"""
import argparse
import io
import json
import os
import socket
import subprocess
import tempfile
import time
import tkinter as tk

from PIL import Image

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
QEMU = os.path.join(ROOT, "build", "qemu", "b", "qemu-system-arm")

# Tk keysym -> QEMU qcode (the board maps qcodes to 3DS buttons, nintendo3ds.c)
KEYS = {"Return": "ret", "KP_Enter": "ret", "h": "ret",
        "BackSpace": "backspace", "k": "backspace",
        "a": "a", "s": "s", "x": "x", "y": "y", "q": "q", "w": "w",
        "Up": "up", "Down": "down", "Left": "left", "Right": "right"}

HELP = ("Enter/H = Home   Backspace/K = Lock   Q/W = Volume -/+   "
        "A S X Y = A B X Y   Arrows = D-pad   (iOS needs ~7 min to boot)")


class QMP:
    def __init__(self, path):
        for _ in range(100):
            try:
                self.s = socket.socket(socket.AF_UNIX)
                self.s.connect(path)
                break
            except OSError:
                time.sleep(0.1)
        self.f = self.s.makefile("rwb")
        self.f.readline()                       # greeting
        self.cmd("qmp_capabilities")

    def cmd(self, name, **args):
        self.f.write(json.dumps({"execute": name, "arguments": args}).encode() + b"\n")
        self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if "return" in r or "error" in r:   # skip async events
                return r

    def key(self, qcode, down):
        self.cmd("input-send-event", events=[{"type": "key", "data": {
            "down": down, "key": {"type": "qcode", "data": qcode}}}])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sd", default=os.path.join(ROOT, "build", "sd.img"))
    ap.add_argument("--firm", default=os.path.join(ROOT, "build", "boot", "ios3ds.firm"))
    ap.add_argument("--fps", type=float, default=8)
    a = ap.parse_args()

    tmp = tempfile.mkdtemp(prefix="ios3ds-view-")
    qmp_path, shot = os.path.join(tmp, "qmp.sock"), os.path.join(tmp, "screen.ppm")
    proc = subprocess.Popen([QEMU, "-M", "nintendo3ds", "-accel", "tcg,thread=multi",
                             "-kernel", a.firm, "-drive", "file=%s,if=sd,format=raw" % a.sd,
                             "-serial", "none", "-display", "none",
                             "-qmp", "unix:%s,server,nowait" % qmp_path])
    q = QMP(qmp_path)

    root = tk.Tk()
    root.title("ios3ds - nintendo3ds QEMU")
    root.configure(bg="black")
    pic = tk.Label(root, bg="black")
    pic.pack()
    tk.Label(root, text=HELP, fg="white", bg="black").pack(fill="x")
    held, pending_up = set(), {}

    def press(ev):
        qc = KEYS.get(ev.keysym) or KEYS.get(ev.keysym.lower())
        if not qc:
            return
        if qc in pending_up:                    # X11 autorepeat: release+press pair
            root.after_cancel(pending_up.pop(qc))
            return
        if qc not in held:
            held.add(qc)
            q.key(qc, True)

    def release(ev):
        qc = KEYS.get(ev.keysym) or KEYS.get(ev.keysym.lower())
        if not qc or qc not in held:
            return

        def up():
            pending_up.pop(qc, None)
            held.discard(qc)
            q.key(qc, False)
        pending_up[qc] = root.after(40, up)

    def frame():
        if proc.poll() is not None:
            root.destroy()
            return
        if q.cmd("screendump", filename=shot).get("return") is not None:
            try:
                im = Image.open(shot).convert("RGB")       # 400x480: top over bottom
                ios = im.crop((20, 0, 380, 240)).rotate(-90, expand=True)  # 240x360 upright
                ios = ios.resize((480, 720), Image.NEAREST)
                ds = im.resize((600, 720), Image.NEAREST)
                out = Image.new("RGB", (480 + 10 + 600, 720))
                out.paste(ios, (0, 0))
                out.paste(ds, (490, 0))
                buf = io.BytesIO()
                out.save(buf, "PPM")
                img = tk.PhotoImage(data=buf.getvalue())
                pic.configure(image=img)
                pic.image = img
            except (OSError, tk.TclError):
                pass
        root.after(int(1000 / a.fps), frame)

    root.bind("<KeyPress>", press)
    root.bind("<KeyRelease>", release)
    root.after(200, frame)
    try:
        root.mainloop()
    finally:
        try:
            q.cmd("quit")
        except Exception:
            pass
        proc.wait(timeout=10)


if __name__ == "__main__":
    main()
