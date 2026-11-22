#!/usr/bin/env python3
"""Driver stress test in QEMU: open/close apps, touch drags, page swipes,
circle pad, lock/unlock, screen rotation, camera start/stop cycles and
headphone jack changes, then check the SD log for panics, hangs and driver
errors.

  python3 emu/stress.py --cycles 5 --icount 2
  python3 emu/stress.py --cycles 10 --icount 2 --camera-cycles 10 --diag
  python3 emu/stress.py --new3ds --cycles 5 --icount 2

Unlike emu/soak.py this uses apps whose place on the home screen is known
(rootfs/recipe.json IconState), so the camera and the audio route get real
start/stop cycles.  The image is disposable: --mem / --diag edit its
sd:/ios3ds/boot.cfg (a fresh image is required per run, as for the soak).

Writes build/emu/stress/report.md + report.json, one screenshot per app and
the final log.  Exit status 1 if a check failed.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

from PIL import Image, ImageChops, ImageStat

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import viewer  # noqa: E402

ROOT = viewer.ROOT

# iOS points on page 1 (rootfs/recipe.json IconState; soak.py's 4x4 grid)
APPS = {
    "photos": (199, 50),
    "notes": (47, 226),
    "settings": (47, 314),
    "camera": (123, 314),
    "safari": (199, 440),       # dock
    "music": (47, 440),         # dock
}

CHECKS = [
    ("kernel panic", r"panic\(cpu", True),
    ("ARM11 stall dump", r"\[probe\] ARM11 stalled", True),
    ("md0 I/O error", r"\[blk\] .* failed", True),
    ("log write error", r"\[log\] write error", True),
    ("md0 errors in heartbeat", r"\[hb\] t=.* (?!0 err)\d+ err$", True),
    ("process crash (signal)", r"exited abnormally|Segmentation fault|Bus error|Abort trap", True),
    ("watchdog / hang", r"watchdog (timeout|reset|fired)|hang detected", True),
    ("GPU rendering lost", r"GPU rendering is OFF|reading surfaces back through the CPU from now on", False),
    ("GPU timeout", r"\[pica\] TIMEOUT", True),
    ("GPU present check failed", r"GPU present check FAILED", True),
    ("camera init failed", r"\[cam\].*init failed|\[cam\].*did not answer", False),
    ("camera FIFO overruns", r"\[cam\].*FIFO overruns [1-9]", False),
    ("audio device error", r"could not start DMA|no CODEC", False),
    ("input stale port", r"\[input\] stale touch port", False),
    ("SpringBoard HID reconnect", r"\[mcud\] SpringBoard's HID event port went away", False),
    ("memory report", r"^\[mem ", False),
    ("thread sampler", r"^\[smp ", False),
    ("gyro updates", r"^\[gyrotest\]", False),
]

# Braces around the probe names make the report readable without a table.
def read_sd_text(sd, path):
    r = subprocess.run(["mtype", "-i", "%s@@%d" % (sd, 2048 * 512), "::" + path],
                       capture_output=True, text=True, errors="replace")
    return r.stdout


def edit_bootcfg(sd, add_diag, add_mem):
    """Append toggles to sd:/ios3ds/boot.cfg inside the FAT image."""
    if not add_diag and not add_mem:
        return
    tmp = tempfile.mkdtemp(prefix="ios3ds-cfg-")
    local = os.path.join(tmp, "boot.cfg")
    base = "%s@@%d" % (sd, 2048 * 512)
    r = subprocess.run(["mcopy", "-o", "-i", base, "::/ios3ds/boot.cfg", local],
                       capture_output=True, text=True)
    if r.returncode:
        print("[stress] cannot read boot.cfg: %s" % r.stderr.strip())
        return
    text = open(local, errors="replace").read()
    if add_diag:
        text = re.sub(r"(?m)^diag\s*=.*$", "diag=1", text)
        if not re.search(r"(?m)^diag\s*=", text):
            text += "\ndiag=1\n"
    if add_mem:
        m = re.search(r"(?m)^cmdline\s*=(.*)$", text)
        if m:
            if "ios3ds_mem=1" not in m.group(1):
                text = text[:m.start(1)] + m.group(1) + " ios3ds_mem=1" + text[m.end(1):]
        else:
            text += "\ncmdline=ios3ds_mem=1\n"
    open(local, "w").write(text)
    r = subprocess.run(["mcopy", "-o", "-i", base, local, "::/ios3ds/boot.cfg"],
                       capture_output=True, text=True)
    if r.returncode:
        print("[stress] cannot write boot.cfg: %s" % r.stderr.strip())
    shutil.rmtree(tmp)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sd", default=os.path.join(ROOT, "build", "sd.img"))
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "emu", "stress"))
    ap.add_argument("--cycles", type=int, default=5)
    ap.add_argument("--camera-cycles", type=int, default=0,
                    help="extra Camera.app start/stop cycles after the main loop (default: cycles)")
    ap.add_argument("--settle", type=float, default=8.0)
    ap.add_argument("--boot-timeout", type=float, default=1200)
    ap.add_argument("--icount", default="2")
    ap.add_argument("--new3ds", action="store_true")
    ap.add_argument("--diag", action="store_true", help="set boot.cfg diag=1 (gltest probe, gyrotest, [smp])")
    ap.add_argument("--no-mem", action="store_true", help="do not add ios3ds_mem=1 (the [mem] report)")
    ap.add_argument("--no-camera", action="store_true")
    ap.add_argument("--rotate", default="1", help="qom mcu-orient poses to cycle, e.g. 1,3,0; empty = off")
    a = ap.parse_args()
    if a.new3ds:
        os.environ["IOS3DS_NEW3DS"] = "1"
    if a.camera_cycles <= 0:
        a.camera_cycles = 0 if a.no_camera else a.cycles
    os.makedirs(a.out, exist_ok=True)
    edit_bootcfg(a.sd, a.diag, not a.no_mem)

    tmp = tempfile.mkdtemp(prefix="ios3ds-stress-")
    sock = os.path.join(tmp, "qmp.sock")
    qout = open(os.path.join(a.out, "qemu.txt"), "w")
    cmd = [viewer.QEMU, "-M", ("nintendo3ds,new3ds=on" if os.environ.get("IOS3DS_NEW3DS") == "1"
                               else "nintendo3ds"),
           "-accel", "tcg,thread=single" if a.icount else "tcg,thread=multi",
           "-kernel", os.path.join(ROOT, "build", "boot", "ios3ds.firm"),
           "-drive", "file=%s,if=sd,format=raw" % a.sd, "-serial", "none",
           "-display", "none", "-qmp", "unix:%s,server,nowait" % sock]
    if a.icount:
        cmd += ["-icount", "shift=%s,sleep=off" % a.icount]
    proc = subprocess.Popen(cmd, stdout=qout, stderr=subprocess.STDOUT)
    q = viewer.QMP(sock)
    t0 = time.time()
    log = lambda m: print("[stress %6.0fs] %s" % (time.time() - t0, m), flush=True)  # noqa: E731

    def shot(name):
        f = os.path.join(tmp, "s.ppm")
        q.cmd("screendump", filename=f)
        img = Image.open(f).crop((20, 0, 380, 240)).rotate(-90, expand=True)
        img.save(os.path.join(a.out, name + ".png"))
        return img

    def settled(name, first=3.0, limit=25.0):
        start = time.time()
        time.sleep(first)
        prev = shot(name)
        bright = ImageStat.Stat(prev.convert("L")).mean[0]
        while time.time() - start < limit:
            time.sleep(1.5)
            img = shot(name)
            b = ImageStat.Stat(img.convert("L")).mean[0]
            if b > 10 and bright > 10 and ImageStat.Stat(ImageChops.difference(
                    img.convert("L"), prev.convert("L"))).mean[0] < 1.0:
                return img
            prev, bright = img, b
        return prev

    def tap(x, y, hold=0.15):
        dx, dy = viewer.ios_to_display(x / 320.0, y / 480.0)
        q.pointer(dx, dy, True)
        time.sleep(hold)
        q.pointer(dx, dy, False)

    def drag(x0, y0, x1, y1, secs=0.5):
        dx, dy = viewer.ios_to_display(x0 / 320.0, y0 / 480.0)
        q.pointer(dx, dy, True)
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

    def qset(prop, value):
        q.cmd("qom-set", path="/machine", property=prop, value=value)

    def open_app(name, wait=None):
        x, y = APPS[name]
        tap(x, y)
        time.sleep(wait if wait is not None else a.settle)
        shot("app_" + name)
        key("ret")
        settled("home_after_" + name)

    # 1. boot
    booted = None
    while time.time() - t0 < a.boot_timeout:
        time.sleep(10)
        if "[input] connected to SpringBoard" in read_sd_text(a.sd, "/ios3ds/log.txt"):
            booted = time.time() - t0
            break
        if proc.poll() is not None:
            break
    results = {"boot_seconds": booted, "cycles": [], "apps": {}, "rotation": [], "checks": {}}
    if booted is None:
        log("boot did not reach SpringBoard in %.0f s" % a.boot_timeout)
    else:
        log("SpringBoard + input up after %.0f s" % booted)
        home = settled("home_ref", first=5.0, limit=60.0)
        ref = home.convert("L")

        def ensure_home(tag):
            """A lost D-pad edge can leave the home screen on page 2: swipe back."""
            for _ in range(3):
                img = shot(tag)
                if ImageStat.Stat(ImageChops.difference(img.convert("L"), ref)).mean[0] <= 12:
                    return
                drag(40, 240, 280, 240, 0.6)      # finger right = previous page
                time.sleep(1.5)

        # 2. main loop: app open/close, touch drags, page swipes, circle pad
        sequence = ["photos", "settings", "notes", "safari", "music"]
        for i in range(a.cycles):
            c = {"cycle": i}
            name = sequence[i % len(sequence)]
            x, y = APPS[name]
            tap(x, y)
            time.sleep(a.settle)
            app = shot("c%03d_%s" % (i, name))
            c["app"] = name
            c["app_lit"] = round(ImageStat.Stat(app.convert("L")).mean[0], 1)
            # continuous drag across the app, then a vertical drag
            drag(160, 240, 40, 240, 0.8)
            drag(160, 160, 160, 380, 0.8)
            shot("c%03d_drag_%s" % (i, name))
            key("ret")
            home = settled("c%03d_home" % i)
            c["home_diff"] = round(ImageStat.Stat(ImageChops.difference(home.convert("L"), ref)).mean[0], 2)
            if c["home_diff"] > 12:
                # Home landed on Spotlight or an alert: recover like soak.py
                for step, act in (("second Home", lambda: key("ret")),
                                  ("Dismiss", lambda: tap(160, 333)),
                                  ("Dismiss + Home", lambda: (tap(160, 333), time.sleep(3), key("ret")))):
                    act()
                    time.sleep(5)
                    h = shot("c%03d_recover" % i)
                    if ImageStat.Stat(ImageChops.difference(h.convert("L"), ref)).mean[0] <= 12:
                        c["recovered_with"] = step
                        break
                else:
                    c["recovered_with"] = "stuck"
            # page swipes with the D-pad (left/right); recover if one is lost
            key("right")
            time.sleep(2)
            key("left")
            time.sleep(2)
            ensure_home("c%03d_pages" % i)
            # circle pad: push down for a second (virtual finger scroll)
            q.key("i", True)
            time.sleep(1.2)
            q.key("i", False)
            time.sleep(1)
            log("cycle %d %s: app %.0f, home diff %.1f%s" %
                (i, name, c["app_lit"], c["home_diff"],
                 "" if "recovered_with" not in c else " (%s)" % c["recovered_with"]))
            results["cycles"].append(c)
            if proc.poll() is not None:
                log("QEMU exited early")
                break

        # 3. camera start/stop cycles (Camera.app at a known home-screen slot)
        if not a.no_camera:
            cam = {"started": 0, "stopped": 0, "lit": []}
            for i in range(a.camera_cycles):
                tap(*APPS["camera"])
                time.sleep(max(10.0, a.settle))
                img = shot("cam%03d" % i)
                cam["lit"].append(round(ImageStat.Stat(img.convert("L")).mean[0], 1))
                text = read_sd_text(a.sd, "/ios3ds/log.txt")
                cam["started"] = max(cam["started"], len(re.findall(r"\[cam\] .*camera streaming", text)))
                key("ret")
                settled("cam_home_%03d" % i)
                cam["stopped"] = max(cam["stopped"], len(re.findall(r"\[cam\] .*cameras off", text)))
                log("camera cycle %d: preview %.0f, starts %d, stops %d" %
                    (i, cam["lit"][-1], cam["started"], cam["stopped"]))
            results["camera"] = cam

        # 4. headphones: plug/unplug a few times while Music is open
        if "music" in APPS:
            tap(*APPS["music"])
            time.sleep(a.settle)
            jack = []
            for i in range(2):
                qset("headphones", True)
                time.sleep(3)
                qset("headphones", False)
                time.sleep(3)
                text = read_sd_text(a.sd, "/ios3ds/log.txt")
                jack.append({"in": len(re.findall(r"\[jack\] headphones plugged in", text)),
                             "out": len(re.findall(r"\[jack\] headphones unplugged", text)),
                             "route": len(re.findall(r"\[audio\] iOS route ->", text))})
                log("jack cycle %d: in %d out %d ios routes %d" % (i, jack[-1]["in"], jack[-1]["out"],
                                                                  jack[-1]["route"]))
            results["jack"] = jack
            key("ret")
            settled("home_after_jack")

        # 5. accelerometer: turn the console; mcud logs SpringBoard's own orientation
        poses = [int(v) for v in a.rotate.split(",") if v.strip()]
        if poses:
            key("ret")
            time.sleep(a.settle)
            for p in poses:
                qset("mcu-orient", p)
                time.sleep(3)
                results["rotation"].append(p)
                log("rotation pose %d" % p)
            qset("mcu-orient", 0)
            time.sleep(3)

        # 6. lock / wake / unlock, twice
        unlocks = []
        for i in range(2):
            key("backspace")
            time.sleep(4)
            key("ret")
            time.sleep(3)
            for attempt in range(3):
                drag(40, 420, 300, 420, 0.8)
                u = settled("unlock_%d" % i)
                if ImageStat.Stat(u.convert("L")).mean[0] > 5:
                    break
            d = ImageStat.Stat(ImageChops.difference(u.convert("L"), ref)).mean[0]
            unlocks.append({"attempts": attempt + 1, "diff": round(d, 2)})
            log("lock round %d: %d unlock attempt(s), diff %.1f" % (i, attempt + 1, d))
        results["unlocks"] = unlocks

    try:
        q.cmd("quit")
        proc.wait(timeout=30)
    except Exception:
        proc.kill()
    qout.close()
    text = read_sd_text(a.sd, "/ios3ds/log.txt")
    open(os.path.join(a.out, "log.txt"), "w").write(text)

    failed = booted is None
    for name, rx, hard in CHECKS:
        hits = [l for l in text.splitlines() if re.search(rx, l)]
        results["checks"][name] = {"count": len(hits), "first": hits[0][:220] if hits else None, "failure": hard}
        if hard and hits:
            failed = True
    # camera sessions must really have run when asked
    cam = results.get("camera")
    if cam and a.camera_cycles:
        if cam["started"] < a.camera_cycles:
            failed = True
        if cam["stopped"] < a.camera_cycles:
            failed = True
    # SB / GPU / app crashes show as "stuck" recovery or a dead screen
    stuck = [c["cycle"] for c in results["cycles"] if c.get("recovered_with") == "stuck"]
    results["stuck_cycles"] = stuck
    if stuck:
        failed = True
    bad_unlock = [i for i, u in enumerate(results.get("unlocks", [])) if u["diff"] > 20]
    results["unlock_not_home"] = bad_unlock
    if bad_unlock:
        failed = True
    # memory: first and last [mem] line, if the report was enabled
    mem = [l for l in text.splitlines() if l.startswith("[mem ")]
    if mem:
        def free_kib(line):
            m = re.search(r"KiB free (\d+)", line)
            return int(m.group(1)) if m else None
        results["mem"] = {"samples": len(mem), "first_free_kib": free_kib(mem[0]),
                          "last_free_kib": free_kib(mem[-1]), "last": mem[-1][:300]}
    hb = [l for l in text.splitlines() if l.startswith("[hb] t=")]
    results["last_heartbeat"] = hb[-1] if hb else None
    results["failed"] = failed

    json.dump(results, open(os.path.join(a.out, "report.json"), "w"), indent=1)
    lines = ["# Driver stress report", "",
             "- boot to SpringBoard + input: %s" % ("%.0f s" % booted if booted else "**not reached**"),
             "- cycles: %d, camera cycles: %d" % (len(results["cycles"]), a.camera_cycles),
             "- stuck cycles (Home did not return): %s" % (stuck or "none"),
             "- unlock rounds not at the home screen: %s" % (bad_unlock or "none")]
    if "camera" in results:
        lines.append("- camera: %d start(s), %d stop(s)" % (results["camera"]["started"],
                                                            results["camera"]["stopped"]))
    if results.get("rotation"):
        lines.append("- accelerometer poses: %s" % results["rotation"])
    if "mem" in results:
        lines.append("- memory: free %s -> %s KiB over %d samples" %
                     (results["mem"]["first_free_kib"], results["mem"]["last_free_kib"],
                      results["mem"]["samples"]))
    if results.get("last_heartbeat"):
        lines.append("- last heartbeat: `%s`" % results["last_heartbeat"])
    lines += ["", "| check | count | first |", "|---|---|---|"]
    for name, r in results["checks"].items():
        lines.append("| %s%s | %d | %s |" % (name, " (failure)" if r["failure"] else "", r["count"],
                                             ("`%s`" % r["first"]) if r["first"] else ""))
    lines += ["", "**Result: %s**" % ("FAIL" if failed else "PASS")]
    open(os.path.join(a.out, "report.md"), "w").write("\n".join(lines) + "\n")
    print("\n".join(lines))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
