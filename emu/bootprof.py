#!/usr/bin/env python3
"""Boot-time and cold app-launch profile under instruction-clocked time.

  python3 emu/bootprof.py                      # --icount 2, Settings, Notes, Safari
  python3 emu/bootprof.py --cfg diag=1 --out build/emu/bootprof-diag
  python3 emu/bootprof.py --report-only --out build/emu/bootprof   # re-analyse

Boots build/sd.img with boot.cfg ioprof=2 log_ts=1 (restored afterwards) and
QEMU's profiler (N3DS_PROF), waits until md0 has been quiet for --quiet guest
seconds after the input daemon is up, then cold-launches each app (tap, wait
for md0 to go quiet, screenshot, Home, wait again). All times are guest time
(ARM9 clock, seconds since the ARM9 started). Writes report.md, report.json,
blk.csv (one md0 chunk per line) and the log to --out. docs/boot-perf.md.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import time

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import viewer  # noqa: E402

ROOT = viewer.ROOT
FAT = 2048 * 512
APPS = "Settings:47,314;Notes:47,226;Safari:199,440"
# boot milestones: (name, regex on a time-stamped console line)
MILESTONES = [
    ("BSD root (kernel up)", r"BSD root: md0"),
    ("launchd", r"launchd\[1\] has started up"),
    ("SpringBoard HID up", r"\[input\] connected to SpringBoard"),
    ("input daemon up", r"\[input\] mailbox up"),
]
TS = re.compile(r"^\[(\d+)\.(\d{3})\] (.*)$")
BLK = re.compile(r"\[blk\] (\d+)\.(\d{3}) ([rw]) (\d+)\+(\d+) buf (\d+) f([0-9a-f]+) (\d+) us")
BOOT = re.compile(r"\[boot\] (\d+)\.(\d{3}) s: (.*)$")
IO = re.compile(r"\[io\] t=(\d+)\.(\d{3}) ")
QP = re.compile(r"^\[qprof\] a9t=([\d.]+) t=([\d.]+) a11 (\d+)% a9 (\d+)% md0 (\d+)% md0\+a11idle (\d+)%")


def mtype(sd, path):
    r = subprocess.run(["mtype", "-i", "%s@@%d" % (sd, FAT), path], capture_output=True)
    return r.stdout.decode(errors="replace")


def mput(sd, path, text):
    with tempfile.NamedTemporaryFile("w", delete=False, newline="") as f:
        f.write(text)
    subprocess.run(["mcopy", "-o", "-i", "%s@@%d" % (sd, FAT), f.name, path], check=True)
    os.unlink(f.name)


def patch_cfg(text, kv):
    lines = text.replace("\r\n", "\n").split("\n")
    for k, v in kv.items():
        for i, l in enumerate(lines):
            if l.startswith(k + "="):
                lines[i] = "%s=%s" % (k, v)
                break
        else:
            lines.insert(len(lines) - 1, "%s=%s" % (k, v))
    return "\r\n".join(lines)


def guest_now(qprof):
    try:
        with open(qprof) as f:
            lines = f.read().splitlines()
    except OSError:
        return 0.0
    for l in reversed(lines):
        m = QP.match(l)
        if m:
            return float(m.group(1))
    return 0.0


def last_io(log):
    t = 0.0
    for l in log.splitlines():
        m = IO.search(l)
        if m:
            t = int(m.group(1)) + int(m.group(2)) / 1000.0
    return t


def run(a, out, qprof):
    apps = [(n, tuple(float(v) for v in xy.split(","))) for n, xy in
            (p.split(":") for p in a.apps.split(";") if p)]
    tmp = tempfile.mkdtemp()
    sock = os.path.join(tmp, "qmp.sock")
    cmd = [viewer.QEMU, "-M", "nintendo3ds", "-accel", "tcg,thread=single",
           "-kernel", os.path.join(ROOT, "build", "boot", "ios3ds.firm"),
           "-drive", "file=%s,if=sd,format=raw" % a.sd, "-serial", "none",
           "-display", "none", "-qmp", "unix:%s,server,nowait" % sock,
           "-icount", "shift=%s,sleep=off" % a.icount, "-rtc", "clock=vm"]
    env = dict(os.environ, N3DS_PROF=qprof)
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
    q = viewer.QMP(sock)
    events = []
    t_host = time.time()

    def wait_quiet(what, need_text=None):
        start = guest_now(qprof)
        while time.time() - t_host < a.timeout:
            time.sleep(3)
            if proc.poll() is not None:
                raise SystemExit("QEMU exited")
            log = mtype(a.sd, "::/ios3ds/log.txt")
            g = guest_now(qprof)
            if need_text and need_text not in log:
                continue
            if g - max(last_io(log), start) >= a.quiet:
                print("[bootprof] %s: quiet at guest %.1f s" % (what, g), flush=True)
                return g
        raise SystemExit("timeout waiting for %s" % what)

    def shot(name):
        f = os.path.join(tmp, "s.ppm")
        q.cmd("screendump", filename=f)
        Image.open(f).crop((20, 0, 380, 240)).rotate(-90, expand=True).save(os.path.join(out, name + ".png"))

    events.append(("boot quiet", wait_quiet("boot", "[input] mailbox up")))
    shot("home")
    for name, (x, y) in apps:
        dx, dy = viewer.ios_to_display(x / 320.0, y / 480.0)
        events.append(("tap " + name, guest_now(qprof)))
        q.pointer(dx, dy, True)
        time.sleep(0.15)
        q.pointer(dx, dy, False)
        events.append((name + " quiet", wait_quiet(name)))
        shot(name)
        q.key("h", True)
        time.sleep(0.2)
        q.key("h", False)
        events.append((name + " home quiet", wait_quiet(name + " home")))
    shot("end")
    q.cmd("quit")
    proc.wait(timeout=30)
    with open(os.path.join(out, "events.json"), "w") as f:
        json.dump(events, f)


def ms(m, i):
    return int(m.group(i)) + int(m.group(i + 1)) / 1000.0


def analyse(out, show=True):
    log = open(os.path.join(out, "log.txt"), errors="replace").read().splitlines()
    qp = []
    if os.path.exists(os.path.join(out, "qprof.txt")):
        for l in open(os.path.join(out, "qprof.txt")):
            m = QP.match(l)
            if m:
                qp.append(tuple(float(v) for v in m.groups()))
    blk, stamps, marks, taps = [], [], {}, []
    last_ts = 0.0
    for l in log:
        m = BLK.search(l)
        if m:
            blk.append((ms(m, 1), m.group(3), int(m.group(4)), int(m.group(5)), int(m.group(6)),
                        int(m.group(7), 16), int(m.group(8))))
            continue
        m = BOOT.search(l)
        if m:
            stamps.append((ms(m, 1), m.group(3)))
            continue
        m = TS.match(l)
        t, text = (ms(m, 1), m.group(3)) if m else (last_ts, l)     # no stamp: the last one seen
        last_ts = t
        if True:
            for name, rx in MILESTONES:
                if name not in marks and re.search(rx, text):
                    marks[name] = t
            if re.search(r"\[input\] down", text):
                taps.append(t)
    with open(os.path.join(out, "blk.csv"), "w") as f:
        f.write("t,op,lba,sectors,buf_bytes,flags,arm9_us\n")
        for b in blk:
            f.write("%.3f,%s,%d,%d,%d,0x%x,%d\n" % b)
    events = json.load(open(os.path.join(out, "events.json"))) if os.path.exists(
        os.path.join(out, "events.json")) else []

    def settled(t0, quiet=1.0):
        """end of the first `quiet` seconds after t0 with the ARM11 < 10 % busy and no md0"""
        run = None
        for p in qp:
            if p[0] <= t0 + 0.25:
                continue
            if p[2] < 10 and p[4] == 0:
                run = run if run is not None else p[0] - 0.25
                if p[0] - run >= quiet:
                    return run
            else:
                run = None
        return None

    def burst_end(t0, gap=1.0):
        """last md0 request after t0 before a gap of `gap` seconds"""
        last = t0
        for b in blk:
            if b[0] < t0:
                continue
            if b[0] - last > gap:
                break
            last = b[0]
        return last

    def stats(t0, t1):
        sel = [b for b in blk if t0 <= b[0] < t1]
        rd = sum(b[3] for b in sel if b[1] == "r") / 2
        wr = sum(b[3] for b in sel if b[1] == "w") / 2
        q = [p for p in qp if t0 < p[0] <= t1 + 0.5]
        avg = lambda i: sum(p[i] for p in q) / len(q) if q else float("nan")
        bufs = [b for b in sel if b[5] & 0x80000000]
        return {
            "t0": round(t0, 3), "t1": round(t1, 3), "secs": round(t1 - t0, 3),
            "xnu_requests": len(bufs), "chunks": len(sel), "read_KiB": rd, "write_KiB": wr,
            "meta_KiB": sum(b[3] for b in sel if b[5] & 0x100) / 2,
            "pagein_KiB": sum(b[3] for b in sel if b[5] & 0x80) / 2,
            "avg_request_KiB": round(sum(b[4] for b in bufs) / 1024.0 / len(bufs), 1) if bufs else 0,
            "arm9_service_ms": round(sum(b[6] for b in sel) / 1000.0),
            "a11_busy_pct": round(avg(2)), "md0_pending_pct": round(avg(4)),
            "a11_idle_waiting_md0_pct": round(avg(5)),
        }

    rows = []
    xnu = next((t for t, w in stamps if w == "XNU started"), None)
    prev = 0.0
    for t, what in stamps:
        rows.append(("ARM9: " + what, prev, t))
        prev = t
    for name, _ in MILESTONES:
        if name in marks:
            rows.append((name, prev, marks[name]))
            prev = marks[name]
    # the boot ends with md0's last activity before the first tap
    ev = dict((k, v) for k, v in events)
    first_tap = next((v for k, v in events if k.startswith("tap ")), None)
    if first_tap:
        st = settled(prev, quiet=3.0)
        last = max([b[0] for b in blk if b[0] < (st or first_tap)] or [prev])
        rows.append(("md0 quiet (last request before settling)", prev, max(last, prev)))
        if st and st > last:
            rows.append(("ARM11 settled (3 s < 10 % busy, no md0)", last, st))
    boot = [dict(stats(t0, t1), phase=n) for n, t0, t1 in rows]
    total = stats(0, rows[-1][2]) if rows else {}
    apps = []
    names = [k[4:] for k, _ in events if k.startswith("tap ")]
    for n in names:
        tap = ev["tap " + n]
        # the tap's own timestamp: the input daemon's first touch-down after it
        down = next((t for t in taps if t >= tap - 0.3), tap)
        io_end = burst_end(down)
        cpu_end = settled(down)
        end = max(io_end, cpu_end or io_end)
        apps.append(dict(stats(down, end), app=n, md0_burst_s=round(io_end - down, 2),
                         settled_s=round(cpu_end - down, 2) if cpu_end else None))
    rep = {"boot": boot, "boot_total": total, "xnu_start": xnu, "apps": apps}
    json.dump(rep, open(os.path.join(out, "report.json"), "w"), indent=1)
    hdr = ("| phase | end (s) | dur (s) | XNU req | chunks | read KiB | meta KiB | pagein KiB | "
           "avg req KiB | ARM9 svc ms | ARM11 busy | md0 pending | ARM11 idle on md0 |\n|---" + "|---" * 12 + "|\n")
    fmt = lambda name, s: "| %s | %.1f | %.1f | %d | %d | %d | %d | %d | %.1f | %d | %s%% | %s%% | %s%% |\n" % (
        name, s["t1"], s["secs"], s["xnu_requests"], s["chunks"], s["read_KiB"], s["meta_KiB"], s["pagein_KiB"],
        s["avg_request_KiB"], s["arm9_service_ms"], s["a11_busy_pct"], s["md0_pending_pct"],
        s["a11_idle_waiting_md0_pct"])
    with open(os.path.join(out, "report.md"), "w") as f:
        f.write("# Boot profile (%s)\n\nGuest time, seconds since the ARM9 started.\n\n## Boot\n\n" % out)
        f.write(hdr)
        for s in boot:
            f.write(fmt(s["phase"], s))
        if total:
            f.write(fmt("**total**", total))
        f.write("\n## Cold app launches (touch-down until both the md0 burst has ended and the ARM11 has "
                "settled)\n\n" + hdr.replace("phase", "app"))
        for s in apps:
            f.write(fmt(s["app"], s))
        f.write("\n| app | md0 burst (s) | ARM11 settled (s) |\n|---|---|---|\n")
        for s in apps:
            f.write("| %s | %s | %s |\n" % (s["app"], s["md0_burst_s"], s["settled_s"]))
    if show:
        print(open(os.path.join(out, "report.md")).read())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sd", default=os.path.join(ROOT, "build", "sd.img"))
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "emu", "bootprof"))
    ap.add_argument("--icount", default="2")
    ap.add_argument("--apps", default=APPS, help="name:x,y;... (iOS points), empty = boot only")
    ap.add_argument("--cfg", action="append", default=[], help="boot.cfg key=value for this run")
    ap.add_argument("--quiet", type=float, default=8, help="guest seconds without md0 = settled")
    ap.add_argument("--timeout", type=float, default=7200, help="host seconds for the whole run")
    ap.add_argument("--report-only", action="store_true")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    if not a.report_only:
        qprof = os.path.join(a.out, "qprof.txt")
        orig = mtype(a.sd, "::/ios3ds/boot.cfg")
        kv = {"ioprof": "2", "log_ts": "1"}
        kv.update(dict(c.split("=", 1) for c in a.cfg))
        mput(a.sd, "::/ios3ds/boot.cfg", patch_cfg(orig, kv))
        subprocess.run(["mdel", "-i", "%s@@%d" % (a.sd, FAT), "::/ios3ds/log.txt"], capture_output=True)
        try:
            run(a, a.out, qprof)
        finally:
            with open(os.path.join(a.out, "log.txt"), "w") as f:
                f.write(mtype(a.sd, "::/ios3ds/log.txt"))
            mput(a.sd, "::/ios3ds/boot.cfg", orig)
    analyse(a.out)


if __name__ == "__main__":
    main()
