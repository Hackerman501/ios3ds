#!/usr/bin/env python3
"""Interactive window for the nintendo3ds QEMU machine (our QEMU build has no
GTK/SDL/VNC display). Starts QEMU headless with a QMP socket, shows the 3DS
screens and an upright view of iOS (the top screen, rotated back), and sends
real key down/up events through QMP.

  emu/viewer.py [--sd build/sd.img] [--firm build/boot/ios3ds.firm] [--fps 8]

Mouse: click/drag on the iOS view (or the 3DS bottom screen) = touch.
Keys (3DS button):  Enter / H = START (iOS Home)   Backspace / K = SELECT (iOS lock)
                    A = A   S = B   X = X (screen mode: top/both/bottom)   Y = Y
                    Q = L (vol -)   W = R (vol +)
                    arrow keys = D-pad   (SELECT+START together = XNU panic dump)
      3DS MCU (docs/mcu.md): F1 = HOME   F2 = POWER (hold 3 s = "held")   - / = = volume slider
                    C = close/open the shell   [ / ] = turn the console 90 degrees (accelerometer)
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
        "Up": "up", "Down": "down", "Left": "left", "Right": "right",
        "F1": "h", "F2": "p", "minus": "minus", "equal": "equal", "c": "c",
        "bracketleft": "bracket_left", "bracketright": "bracket_right"}

HELP = ("Mouse = touch (iOS view or bottom screen)   Enter/H = Home   Backspace/K = Lock   Q/W = Volume -/+   "
        "X = screen mode   A S Y = A B Y   Arrows = D-pad   F1/F2 = HOME/POWER   -/= = volume slider   "
        "C = shell   [/] = turn   (iOS needs ~7 min to boot)")


def ios_to_display(nx, ny):
    """Normalised iOS point (0..1, portrait, origin top-left) -> pixel on the
    400x480 QEMU display (bottom-screen touch panel at x 40..359, y 240..479).
    Inverse of map_touch() in tools/ios3ds-input/daemon/input.c with `flipx`
    (the ARM9 shows iOS rotated on the top screen; the panel shares its axes)."""
    px = ny * 320.0
    py = (240.0 - 640.0 / 3.0) / 2.0 + (1.0 - nx) * (640.0 / 3.0)
    return 40.0 + px, 240.0 + py


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

    def pointer(self, x, y, down=None):
        """x, y: pixel on the 400x480 display; down: None = move only"""
        ev = [{"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / 400)}},
              {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / 480)}}]
        if down is not None:
            ev.append({"type": "btn", "data": {"down": down, "button": "left"}})
        self.cmd("input-send-event", events=ev)

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

    # A drag belongs to the view it started in: later motion is clamped to that
    # view and the release is ALWAYS sent. (A release outside the views used to
    # be dropped, leaving QEMU's pen down: iOS saw a finger that never lifted.)
    drag = {"view": None}

    def view_at(ev):
        if ev.x < 480 and ev.y < 720:
            return "ios"                         # upright iOS view (2x)
        if ev.x >= 490 and ev.y < 720:
            return "ds"                          # raw 3DS screens (1.5x)
        return None

    def to_display(view, ev):
        y = min(max(ev.y, 0), 719)
        if view == "ios":
            return ios_to_display(min(max(ev.x, 0), 479) / 480.0, y / 720.0)
        return (min(max(ev.x, 490), 1089) - 490) / 1.5, y / 1.5

    def mouse_down(ev):
        drag["view"] = view_at(ev)
        if drag["view"]:
            q.pointer(*to_display(drag["view"], ev), down=True)

    def mouse_move(ev):
        if drag["view"]:
            q.pointer(*to_display(drag["view"], ev))

    def mouse_up(ev):
        if drag["view"]:
            q.pointer(*to_display(drag["view"], ev), down=False)
            drag["view"] = None

    pic.bind("<ButtonPress-1>", mouse_down)
    pic.bind("<B1-Motion>", mouse_move)
    pic.bind("<ButtonRelease-1>", mouse_up)
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
