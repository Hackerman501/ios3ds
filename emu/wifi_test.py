#!/usr/bin/env python3
"""End-to-end Wi-Fi test in QEMU (docs/wifi.md section 7).

  emu/wifi_test.py [--time 450] [--icount 2] [--case ok|nofw|nocfg|wrongpsk|open]
                   [--port 8080] [--shots 400] [--out build/emu-wifi]

Puts sd:/ios3ds/wifi.cfg and a *dummy* firmware container (random bytes in
the layout tools/ios3ds-wifi/nwmfw.py writes; the QEMU model never runs
firmware) into build/sd.img, starts a small HTTP server on the host
(reachable from iOS as http://10.0.2.2:<port>/ through slirp), boots with
the ctr-wifi model's access point bridged to "-nic user", then checks the
log for every step: SDIO -> BMI -> HTC/WMI -> scan -> associate -> 4-way ->
keys -> iOS interface -> DHCP lease -> DNS -> HTTP fetch.

The files are removed from the image afterwards (so the image keeps the
"no wifi.cfg" state), unless --keep.

Cases: ok (WPA2, everything must pass), open (open network), nocfg (no
wifi.cfg: Wi-Fi must stay off and nothing may touch the card), nofw (no
firmware file: one log line, Wi-Fi off), wrongpsk (the 4-way handshake must
fail at message 2, no link).
"""
import argparse
import http.server
import os
import random
import re
import subprocess
import sys
import threading

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools", "ios3ds-wifi"))
import nwmfw  # noqa: E402

SSID = "ios3ds-qemu"
PSK = "ios3ds-test-passphrase"
TOKEN = "ios3ds-wifi-test-%08x" % random.getrandbits(32)
PART = 2048 * 512
hits = []


def mtools(args, sd, check=True):
    return subprocess.run(args[:1] + ["-i", "%s@@%d" % (sd, PART)] + args[1:],
                          capture_output=True, check=check)


def dummy_fw(path):
    rnd = random.Random(1)
    parts = {"stub_data": bytes(rnd.getrandbits(8) for _ in range(0x38)),
             "stub_code": bytes(rnd.getrandbits(8) for _ in range(0x316)),
             "database": bytes(rnd.getrandbits(8) for _ in range(0x1E8)),
             "main": bytes(rnd.getrandbits(8) for _ in range(0x9A00))}
    with open(path, "wb") as f:
        f.write(nwmfw.pack(parts))


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        hits.append(self.path)
        body = ("%s path=%s\n" % (TOKEN, self.path)).encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        print("[http] %s %s" % (self.address_string(), fmt % args))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--case", default="ok",
                    choices=("ok", "open", "nocfg", "nofw", "wrongpsk", "nofifo", "hidden", "tkip"))
    ap.add_argument("--time", type=float, default=450)
    ap.add_argument("--icount", default="")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--sd", default=os.path.join(ROOT, "build", "sd.img"))
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "emu-wifi"))
    ap.add_argument("--shots", default="")
    ap.add_argument("--dns", default="example.com", help="name iOS resolves in the self-test")
    ap.add_argument("--debug", default="1", help="ctr-wifi model log level (0-3)")
    ap.add_argument("--keep", action="store_true", help="leave wifi.cfg / wifi-fw.bin on the image")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    cfg = os.path.join(a.out, "wifi.cfg")
    fw = os.path.join(a.out, "wifi-fw.bin")
    for f in ("wifi.cfg", "wifi-fw.bin"):
        mtools(["mdel", "::/ios3ds/" + f], a.sd, check=False)
    if a.case != "nocfg":
        lines = ["ssid=" + SSID, "verbose=1",
                 "nettest=http://10.0.2.2:%d/ios3ds-nettest %s" % (a.port, a.dns)]
        if a.case == "open":
            lines.append("security=open")
        else:
            lines.append("psk=" + (PSK if a.case != "wrongpsk" else "not-the-passphrase"))
        with open(cfg, "w") as f:
            f.write("\n".join(lines) + "\n")
        mtools(["mcopy", "-o", cfg, "::/ios3ds/wifi.cfg"], a.sd)
        if a.case != "nofw":
            dummy_fw(fw)
            mtools(["mcopy", "-o", fw, "::/ios3ds/wifi-fw.bin"], a.sd)

    srv = http.server.ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    print("[wifi-test] case %s, HTTP on 127.0.0.1:%d (iOS: 10.0.2.2:%d), token %s"
          % (a.case, a.port, a.port, TOKEN))

    extra = ["-nic", "user,model=ctr-wifi",
             "-global", "ctr-wifi.ssid=" + SSID,
             "-global", "ctr-wifi.psk=" + PSK,
             "-global", "ctr-wifi.debug=" + a.debug]
    if a.case == "open":
        extra += ["-global", "ctr-wifi.security=open"]
    if a.case == "nofifo":
        extra += ["-global", "ctr-wifi.fifo32=off"]
    if a.case == "hidden":
        extra += ["-global", "ctr-wifi.hidden=on"]
    if a.case == "tkip":
        extra += ["-global", "ctr-wifi.group=tkip"]
    cmd = [sys.executable, os.path.join(ROOT, "emu", "run.py"), "--time", str(a.time),
           "--sd", a.sd, "--out", a.out, "--extra", " ".join(extra)]
    if a.icount:
        cmd += ["--icount", a.icount]
    if a.shots:
        cmd += ["--shots", a.shots]
    r = subprocess.run(cmd, capture_output=True, text=True)
    out = r.stdout + r.stderr
    with open(os.path.join(a.out, "run.txt"), "w") as f:
        f.write(out)
    srv.shutdown()
    if not a.keep:
        for f in ("wifi.cfg", "wifi-fw.bin"):
            mtools(["mdel", "::/ios3ds/" + f], a.sd, check=False)

    def has(pat):
        return re.search(pat, out, re.M) is not None

    checks = {
        "ok": [
            ("SDIO up, AR6014", r"\[wifi\] SDIO up, chip id 0d000001"),
            ("FIFO check", r"\[wifi\] TMIO2 32-bit FIFO works"),
            ("BMI ROM version", r"\[wifi\] BMI ROM version 2300006f"),
            ("model: firmware running", r"ctr-wifi: .*firmware running"),
            ("WMI ready", r"\[wifi\] WMI ready, MAC"),
            ("scan found the AP", r"ctr-wifi: .*BSSINFO"),
            ("associated", r"ctr-wifi: .*associated with"),
            ("4-way: message 4 OK", r"ctr-wifi: .*message 4 OK"),
            ("pairwise key = AP TK", r"pairwise.*equals the AP's TK"),
            ("group key = AP GTK", r"group.*equals the AP's GTK"),
            ("ARM9 link up", r"\[wifi\] link up"),
            ("uplink traffic", r"ctr-wifi: .*first frame from the station"),
            ("iOS interface", r"\[wifid\] interface is en\d+"),
            ("DHCP lease", r"\[nettest\] .*address 10\.0\.2\.\d+"),
            ("default route", r"\[nettest\] default route via 10\.0\.2\.2"),
            ("DNS", r"\[nettest\] dns " + re.escape(a.dns) + r" -> \d+\.\d+\.\d+\.\d+"),
            ("self-test passed", r"\[nettest\] all steps passed"),
            ("HTTP fetch", r"\[nettest\] http .*" + TOKEN),
        ],
        "open": [
            ("associated (open)", r"ctr-wifi: .*associated with"),
            ("ARM9 link up", r"\[wifi\] link up"),
            ("DHCP lease", r"\[nettest\] .*address 10\.0\.2\.\d+"),
            ("HTTP fetch", r"\[nettest\] http .*" + TOKEN),
        ],
        "nofifo": [
            ("FIFO check fails", r"\[wifi\] TMIO2 32-bit FIFO check failed"),
            ("model: firmware running", r"ctr-wifi: .*firmware running"),
            ("4-way: message 4 OK", r"ctr-wifi: .*message 4 OK"),
            ("ARM9 link up", r"\[wifi\] link up"),
        ],
        "hidden": [
            ("probe response found", r"ctr-wifi: .*BSSINFO.*probe response"),
            ("ARM9 link up", r"\[wifi\] link up"),
        ],
        "tkip": [
            ("TKIP group seen", r"\[wifi\] connect .*TKIP group"),
            ("group key = AP GTK", r"group.*equals the AP's GTK"),
            ("ARM9 link up", r"\[wifi\] link up"),
        ],
        "nocfg": [
            ("Wi-Fi off", r"\[wifi\] no 0:/ios3ds/wifi\.cfg: networking off"),
        ],
        "nofw": [
            ("firmware missing logged", r"\[wifi\] .*wifi-fw\.bin: not found"),
            ("Wi-Fi off", r"\[wifi\] backend ar6014 not available: networking off"),
        ],
        "wrongpsk": [
            ("message 2 rejected", r"ctr-wifi: .*message 2 MIC is wrong"),
            ("deauth after retries", r"ctr-wifi: .*deauthenticating"),
        ],
    }[a.case]
    forbidden = {
        "nocfg": [("card untouched", r"ctr-wifi: .*card powered")],
        "nofw": [("card untouched", r"ctr-wifi: .*card powered")],
        "wrongpsk": [("no link", r"\[wifi\] link up")],
    }.get(a.case, [])
    forbidden += [("no kernel panic", r"panic\(cpu"), ("no ARM9 crash", r"ARM9 (data abort|exception)")]
    ok = True
    for name, pat in checks:
        good = has(pat)
        ok &= good
        print("[wifi-test] %-26s %s" % (name, "PASS" if good else "FAIL"))
    for name, pat in forbidden:
        good = not has(pat)
        ok &= good
        print("[wifi-test] %-26s %s" % (name, "PASS" if good else "FAIL"))
    if a.case in ("ok", "open"):
        print("[wifi-test] host HTTP requests: %s" % (hits or "none"))
    print("[wifi-test] %s (log: %s)" % ("PASS" if ok else "FAIL", os.path.join(a.out, "run.txt")))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
