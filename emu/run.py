#!/usr/bin/env python3
"""Run the ios3ds payload in the QEMU 'nintendo3ds' machine, headless.

  emu/run.py [--time 8] [--shots 2,8] [--gdb] [--trace] [--keys "1.0:s_down"]

Takes screenshots (PNG, top screen above bottom screen) at the given times,
quits, then prints sd:/ios3ds/log.txt from the SD image. Artifacts go to
build/emu/.
"""
import argparse
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = os.path.join(ROOT, "build", "qemu", "b", "qemu-system-arm")
OUT = os.path.join(ROOT, "build", "emu")


def hmp(sock_path, cmd, timeout=5):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(sock_path)
    time.sleep(0.05)
    try:
        s.recv(65536)
    except socket.timeout:
        pass
    s.sendall((cmd + "\n").encode())
    time.sleep(0.3)
    out = b""
    try:
        out = s.recv(65536)
    except socket.timeout:
        pass
    s.close()
    return out.decode(errors="replace")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--firm", default=os.path.join(ROOT, "build", "boot", "ios3ds.firm"))
    ap.add_argument("--sd", default=os.path.join(ROOT, "build", "sd.img"))
    ap.add_argument("--time", type=float, default=8.0)
    ap.add_argument("--shots", default="")
    ap.add_argument("--gdb", action="store_true", help="-s -S: wait for gdb on :1234")
    ap.add_argument("--trace", default="", help="QEMU -d flags, e.g. unimp,guest_errors,int")
    ap.add_argument("--keys", default="", help="'t:key_down,t:key_up' e.g. 0.5:s_down (s = B button)")
    ap.add_argument("--extra", default="")
    ap.add_argument("--regs", action="store_true", help="dump ARM11/ARM9 registers before quitting")
    a = ap.parse_args()

    os.makedirs(OUT, exist_ok=True)
    mon = os.path.join(OUT, "monitor.sock")
    if os.path.exists(mon):
        os.remove(mon)
    cmd = [QEMU, "-M", "nintendo3ds", "-accel", "tcg,thread=multi", "-kernel", a.firm, "-display", "none",
           "-monitor", "unix:%s,server,nowait" % mon, "-serial", "none"]
    if os.path.exists(a.sd):
        cmd += ["-drive", "file=%s,if=sd,format=raw" % a.sd]
    if a.trace:
        cmd += ["-d", a.trace, "-D", os.path.join(OUT, "qemu.log")]
    if a.gdb:
        cmd += ["-s", "-S"]
    if a.extra:
        cmd += a.extra.split()
    print("[run] " + " ".join(cmd))
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    for _ in range(50):
        if os.path.exists(mon):
            break
        time.sleep(0.1)

    events = []
    for s in filter(None, a.shots.split(",")):
        events.append((float(s), "shot", None))
    for k in filter(None, a.keys.split(",")):
        t, act = k.split(":")
        key, updown = act.rsplit("_", 1)
        events.append((float(t), "key", (key, updown)))
    events.append((a.time, "quit", None))
    events.sort(key=lambda e: e[0])

    t0 = time.time()
    for t, kind, arg in events:
        dt = t - (time.time() - t0)
        if dt > 0:
            time.sleep(dt)
        if proc.poll() is not None:
            break
        if kind == "shot":
            png = os.path.join(OUT, "screen_%05.1fs.png" % t)
            hmp(mon, "screendump %s -f png" % png)
            print("[run] screenshot %s" % png)
        elif kind == "key":
            key, updown = arg
            # HMP has no key-down/up; sendkey with a hold time approximates it
            if updown == "down":
                hmp(mon, "sendkey %s 3000" % key.replace("+", "-"))
        elif kind == "quit":
            if a.regs:
                for cpu in (0, 1):
                    hmp(mon, "cpu %d" % cpu)
                    r = hmp(mon, "info registers")
                    with open(os.path.join(OUT, "regs_cpu%d.txt" % cpu), "w") as f:
                        f.write(r)
                    print("[run] cpu%d registers:\n%s" % (cpu, r))
            hmp(mon, "quit")
    try:
        out, _ = proc.communicate(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
        out, _ = proc.communicate()
    print(out.decode(errors="replace").strip())

    if os.path.exists(a.sd):
        r = subprocess.run(["mtype", "-i", "%s@@%d" % (a.sd, 2048 * 512), "::/ios3ds/log.txt"],
                           capture_output=True)
        log = r.stdout.decode(errors="replace")
        with open(os.path.join(OUT, "log.txt"), "w") as f:
            f.write(log)
        print("----- sd:/ios3ds/log.txt -----")
        print(log if log else "(missing: %s)" % r.stderr.decode().strip())


if __name__ == "__main__":
    main()
