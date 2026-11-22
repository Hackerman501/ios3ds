#!/usr/bin/env python3
"""Stability soak test: boot iOS in QEMU, then cycle through the home screen
for a long time (open every icon, Home, page swipes, lock/unlock) and check
the SD log for trouble afterwards.

  python3 emu/soak.py --cycles 40                # ~35 min after boot
  python3 emu/soak.py --cycles 10 --icount 2     # hardware timing (slower)

Writes build/emu/soak/report.md + report.json, one screenshot per cycle
(upright iOS view) and the final log. Exit status 1 if any check failed.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import time

from PIL import ImageFilter, Image, ImageChops, ImageStat

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import viewer  # noqa: E402

ROOT = viewer.ROOT

# iOS points (320x480 portrait): 4x4 icon grid + 4 dock slots on page 1
GRID = [(47 + 76 * c, 50 + 88 * r) for r in range(4) for c in range(4)]
DOCK = [(47 + 76 * c, 440) for c in range(4)]
TARGETS = GRID + DOCK

# log patterns: (name, regex, is_failure)
CHECKS = [
    ("kernel panic", r"panic\(cpu|Debugger message: Fatal Exception|undefined kernel instruction", True),
    ("ARM11 stall dump", r"\[probe\] ARM11 stalled", True),
    ("md0 I/O error", r"\[blk\] .* failed", True),
    ("log write error", r"\[log\] write error", True),
    ("md0 errors in heartbeat", r"\[hb\] t=.* (?!0 err)\d+ err$", True),
    ("process crash (signal)", r"exited abnormally|Segmentation fault|Bus error|Abort trap", True),
    ("jetsam / low memory", r"jetsam|memorystatus|low memory|Received memory warning", False),
    ("SpringBoard restart", r"\[input\] connected to SpringBoard", False),
    ("GPU mirror failure", r"\[gpu\] display mirror( FAILED|: self-test (FAILED|aborted))", False),
    ("GPU timeout", r"\[pica\] TIMEOUT|\[gpu\] display mirror.*did not finish", True),
    ("GPU present check failed", r"GPU present check FAILED", True),
    ("GPU fell back to the CPU", r"GPU rendering is OFF|reading surfaces back through the CPU from now on", False),
    ("camera init failed", r"\[cam\].*init failed|\[cam\].*did not answer", False),
    ("camera FIFO overruns", r"\[cam\].*FIFO overruns [1-9]", False),
    ("audio DMA error", r"could not start DMA|no CODEC", False),
    ("watchdog / hang", r"watchdog (timeout|reset|fired)|hang detected", True),
]


def read_log(sd):
    r = subprocess.run(["mtype", "-i", "%s@@%d" % (sd, 2048 * 512), "::/ios3ds/log.txt"],
                       capture_output=True, text=True, errors="replace")
    return r.stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sd", default=os.path.join(ROOT, "build", "sd.img"))
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "emu", "soak"))
    ap.add_argument("--cycles", type=int, default=40)
    ap.add_argument("--boot-timeout", type=float, default=1200)
    ap.add_argument("--settle", type=float, default=15, help="seconds an app gets after its tap")
    ap.add_argument("--new3ds", action="store_true", help="model a New 3DS (-M nintendo3ds,new3ds=on; env IOS3DS_NEW3DS=1 does the same, docs/n3ds.md)")
    ap.add_argument("--icount", default="")
    ap.add_argument("--long-lock", type=float, default=130, help="seconds locked on every 10th cycle")
    ap.add_argument("--targets", default="", help="x,y;x,y;... iOS points to tap instead of the icon grid + dock")
    ap.add_argument("--settles", default="", help="comma list of per-cycle settle seconds (cycled)")
    a = ap.parse_args()
    if a.new3ds:
        os.environ["IOS3DS_NEW3DS"] = "1"
    os.makedirs(a.out, exist_ok=True)
    targets = [tuple(float(v) for v in p.split(",")) for p in a.targets.split(";") if p] or TARGETS
    settles = [float(v) for v in a.settles.split(",") if v] or [a.settle]
    tmp = tempfile.mkdtemp(prefix="ios3ds-soak-")
    sock = os.path.join(tmp, "qmp.sock")
    accel = "tcg,thread=single" if a.icount else "tcg,thread=multi"
    cmd = [viewer.QEMU, "-M", ("nintendo3ds,new3ds=on" if os.environ.get("IOS3DS_NEW3DS") == "1" else "nintendo3ds"), "-accel", accel,
           "-kernel", os.path.join(ROOT, "build", "boot", "ios3ds.firm"),
           "-drive", "file=%s,if=sd,format=raw" % a.sd, "-serial", "none",
           "-display", "none", "-qmp", "unix:%s,server,nowait" % sock]
    if a.icount:
        cmd += ["-icount", "shift=%s,sleep=off" % a.icount]
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    q = viewer.QMP(sock)
    t0 = time.time()
    log = lambda m: print("[soak %6.0fs] %s" % (time.time() - t0, m), flush=True)  # noqa: E731

    def shot(name):
        f = os.path.join(tmp, "s.ppm")
        q.cmd("screendump", filename=f)
        img = Image.open(f).crop((20, 0, 380, 240)).rotate(-90, expand=True)
        img.save(os.path.join(a.out, name + ".png"))
        return img

    def settled(name, first=4.0, limit=30.0):
        """screenshot once two shots 2 s apart (after `first` s) are the same
        and lit: a still-black screen (backlight fading in after an unlock)
        would otherwise pass here and fail the unlock comparison"""
        t_start = time.time()
        time.sleep(first)
        prev = shot(name)
        bright = ImageStat.Stat(prev.convert("L")).mean[0]
        while time.time() - t_start < limit:
            time.sleep(2)
            img = shot(name)
            b = ImageStat.Stat(img.convert("L")).mean[0]
            if b > 10 and bright > 10 and ImageStat.Stat(ImageChops.difference(
                    img.convert("L"), prev.convert("L"))).mean[0] < 1.0:
                return img, time.time() - t_start
            prev, bright = img, b
        return prev, time.time() - t_start

    def tap(x, y, hold=0.15):
        dx, dy = viewer.ios_to_display(x / 320.0, y / 480.0)
        q.pointer(dx, dy, True)
        time.sleep(hold)
        q.pointer(dx, dy, False)

    def drag(x0, y0, x1, y1, secs=0.4):
        dx, dy = viewer.ios_to_display(x0 / 320.0, y0 / 480.0)
        q.pointer(dx, dy, True)
        # hold the start a moment: with --icount (sleep=off) an idle guest's
        # clock jumps ahead, and a 0.4 s wall-clock swipe was first sampled
        # 50 points in (re-log 77: "down 88,240" for a swipe from 40)
        time.sleep(0.15)
        for i in range(1, 11):
            time.sleep(secs / 10)
            f = i / 10.0
            dx, dy = viewer.ios_to_display((x0 + (x1 - x0) * f) / 320.0, (y0 + (y1 - y0) * f) / 480.0)
            q.pointer(dx, dy, None)
        time.sleep(0.05)
        q.pointer(dx, dy, False)

    def key(qcode, hold=0.2):
        q.key(qcode, True)
        time.sleep(hold)
        q.key(qcode, False)

    # 1. boot: wait for the input daemon to reach SpringBoard
    booted = None
    while time.time() - t0 < a.boot_timeout:
        time.sleep(10)
        if "[input] connected to SpringBoard" in read_log(a.sd):
            booted = time.time() - t0
            break
        if proc.poll() is not None:
            break
    results = {"boot_seconds": booted, "cycles": [], "checks": {}}
    if booted is None:
        log("boot did not reach SpringBoard in %.0f s" % a.boot_timeout)
    else:
        log("SpringBoard + input up after %.0f s; waiting for a stable home screen" % booted)
        # reference: not black (the screen copy may be switching to the GPU,
        # soak 3) and unchanged across two shots 5 s apart.  On a loaded host
        # with --icount 2 SpringBoard's first lit frame came ~220 s after the
        # input daemon connected: allow 300 s (a black reference makes every
        # later diff meaningless and the unlock checks fail).
        prev, home_ref = None, None
        for _ in range(60):
            time.sleep(5)
            img = shot("home_ref")
            bright = ImageStat.Stat(img.convert("L")).mean[0]
            if prev is not None and bright > 20 and ImageStat.Stat(ImageChops.difference(
                    img.convert("L"), prev.convert("L"))).mean[0] < 1.0:
                home_ref = img
                break
            prev = img
        if home_ref is None:
            log("no stable, non-black home screen within 300 s")
            home_ref = prev
        for i in range(a.cycles):
            x, y = targets[i % len(targets)]
            c = {"cycle": i, "tap": [x, y]}
            tap(x, y)
            c["settle"] = settles[i % len(settles)]
            time.sleep(c["settle"])
            app = shot("c%03d_app" % i)
            key("ret")                          # Home (START)
            # wait until the screen stops changing (up to 30 s) instead of a
            # fixed 6 s: with --icount the guest runs slower when the host is
            # busy (other emulators), and a fixed wait screenshots the close
            # animation halfway, then the recovery's second Home opens Spotlight
            home, t_wait = settled("c%03d_home" % i)
            c["home_wait"] = round(t_wait, 1)
            diff = ImageStat.Stat(ImageChops.difference(home.convert("L"), home_ref.convert("L"))).mean[0]
            c["home_diff"] = round(diff, 2)
            c["app_diff"] = round(ImageStat.Stat(ImageChops.difference(
                app.convert("L"), home_ref.convert("L"))).mean[0], 2)
            # not home: Home on page 1 opens Spotlight (a second Home returns);
            # a Home press seen as a hold shows the "Edit Home Screen" alert
            # (needs Dismiss). Recover, but record what it took.
            def dist():
                return ImageStat.Stat(ImageChops.difference(
                    shot("c%03d_recover" % i).convert("L"), home_ref.convert("L"))).mean[0]
            if diff > 12:
                c["recovered_with"] = "stuck"
                for step, act in (("second Home", lambda: key("ret")),
                                  ("Dismiss", lambda: tap(160, 333)),
                                  # On a slow --icount 2 host a cycle can take
                                  # longer than iOS' auto-lock: Home wakes the
                                  # lock screen, the slide returns to the app
                                  # or the home screen.
                                  ("Home + unlock", lambda: (key("ret"), time.sleep(2),
                                                             drag(40, 420, 300, 420, 0.8))),
                                  ("Dismiss + Home", lambda: (tap(160, 333), time.sleep(3), key("ret")))):
                    act()
                    time.sleep(6)
                    if dist() <= 12:
                        c["recovered_with"] = step
                        break
            if i % 4 == 3:                      # page swipes there and back
                drag(280, 240, 40, 240)
                time.sleep(3)
                drag(40, 240, 280, 240)
                # until the page has stopped moving: a tap on a scrolling page
                # only stops it, and with the CA frames presented on the GPU
                # QEMU's PICA200 model makes a scroll take longer than 4 s of
                # wall time (re-log 77)
                settled("c%03d_swipe" % i)
            if i % 5 == 4:                      # lock, wake, slide to unlock
                key("backspace")
                # every 10th cycle a long lock: iOS used to put the whole
                # system to sleep ~1 min after a lock with no way to wake it
                c["lock_seconds"] = a.long_lock if i % 10 == 9 else 4
                time.sleep(c["lock_seconds"])
                key("ret")
                time.sleep(3)
                # The first slide after a wake can be swallowed while the
                # input daemon is still polling at 10 Hz (re-log 75/78): the
                # screen stays black not because iOS hung, but because the
                # gesture never reached SpringBoard. Retry a missed slide
                # before measuring the unlock screen.
                for attempt in range(3):
                    drag(40, 420, 300, 420, 0.8)
                    settled("c%03d_unlock" % i)  # the icons fly in after an unlock
                    u0 = shot("c%03d_unlock" % i).convert("L")
                    if ImageStat.Stat(u0).mean[0] > 5:
                        break
                c["unlock_attempts"] = attempt + 1
                # iOS fades the screen in after an unlock (and the emulated
                # backlight dims it): compare at the reference's brightness so
                # only content differences count (re-log 63)
                # blur: the GPU and the ARM9 CPU screen copy scale slightly
                # differently (after a wake either may be showing)
                u = u0.filter(ImageFilter.GaussianBlur(2))
                ref = home_ref.convert("L").filter(ImageFilter.GaussianBlur(2))
                mu, mr = ImageStat.Stat(u).mean[0], ImageStat.Stat(ref).mean[0]
                if mu > 5:
                    u = u.point(lambda v, k=mr / mu: min(255, int(v * k + 0.5)))
                c["after_unlock_diff"] = round(ImageStat.Stat(ImageChops.difference(u, ref)).mean[0], 2)
            log("cycle %d tap (%d,%d): app diff %.1f, home diff %.1f (home settled after %.0f s)" % (i, x, y, c["app_diff"], diff, c["home_wait"]))
            results["cycles"].append(c)
            if proc.poll() is not None:
                log("QEMU exited early")
                break
    try:
        q.cmd("quit")
        proc.wait(timeout=30)
    except Exception:
        proc.kill()
    text = read_log(a.sd)
    with open(os.path.join(a.out, "log.txt"), "w") as f:
        f.write(text)
    failed = booted is None
    for name, rx, is_fail in CHECKS:
        hits = [l for l in text.splitlines() if re.search(rx, l)]
        results["checks"][name] = {"count": len(hits), "first": hits[0][:200] if hits else None,
                                   "failure": is_fail}
        if is_fail and hits:
            failed = True
    hb = [l for l in text.splitlines() if l.startswith("[hb] t=")]
    results["last_heartbeat"] = hb[-1] if hb else None
    # home screen not restored after Home = stuck app or broken compositor
    rec = {}
    for c in results["cycles"]:
        if "recovered_with" in c:
            rec.setdefault(c["recovered_with"], []).append(c["cycle"])
    results["recoveries"] = rec
    stuck = rec.get("stuck", [])
    if stuck or len(rec.get("Dismiss + Home", [])) > 1:
        failed = True
    # a lock round must end on the home screen (after a long lock too)
    bad_unlock = [c["cycle"] for c in results["cycles"] if c.get("after_unlock_diff", 0) > 20]
    results["unlock_not_home"] = bad_unlock
    if bad_unlock:
        failed = True
    results["failed"] = failed
    with open(os.path.join(a.out, "report.json"), "w") as f:
        json.dump(results, f, indent=1)
    lines = ["# Soak report", "", "- boot to SpringBoard + input: %s" %
             ("%.0f s" % booted if booted else "**not reached**"),
             "- cycles run: %d" % len(results["cycles"]),
             "- not home after Home (cycle lists by what it took to recover): %s" % (rec or "never"),
             "- lock rounds not ending on the home screen (cycles 9, 19, ... lock for %g s): %s" % (a.long_lock, bad_unlock or "none"),
             "- last heartbeat: `%s`" % results["last_heartbeat"], "", "| check | count | first |", "|---|---|---|"]
    for name, r in results["checks"].items():
        lines.append("| %s%s | %d | %s |" % (name, " (failure)" if r["failure"] else "", r["count"],
                                             ("`%s`" % r["first"]) if r["first"] else ""))
    lines += ["", "**Result: %s**" % ("FAIL" if failed else "PASS")]
    with open(os.path.join(a.out, "report.md"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
