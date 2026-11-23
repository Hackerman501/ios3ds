#!/usr/bin/env python3
"""Measure Cydia/APT + dpkg time in QEMU against a local HTTP repository.

Everything the guest installs is built here from scratch (one small .deb, no
third-party downloads) and served from the host through slirp at
http://10.0.2.2:<random port>/repo/. A temporary launch daemon in the image
runs the three measured phases after en0 has an address and prints
`[cperf] ... start/end` lines to the guest console (with log_ts=1 in boot.cfg
the ARM9 timestamps them):

    refresh   apt-get update against the local repo (DNS, connect, Packages)
    download  apt-get --download-only install (transfer + apt parsing)
    install   dpkg -i the single .deb (unpack + fsync on md0)

Run on a FRESH image:

    rm -f build/sd.img && make sdimg
    python3 emu/cydia_perf.py --icount 2 --out build/cydia-perf-before

The image is modified only before the boot (wifi.cfg, boot.cfg log_ts=1, the
source list, the .deb and the test daemon); QEMU is stopped with QMP when the
script reports ALL DONE. The .deb has many small files on purpose: dpkg does
one fsync per unpacked file on a real jailbroken device, which is the cost
this harness is meant to expose.
"""
import argparse
import gzip
import hashlib
import http.server
import os
import random
import shutil
import subprocess
import tempfile
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "emu"))
sys.path.insert(0, os.path.join(ROOT, "tools", "ios3ds-wifi"))
import nwmfw  # noqa: E402
import viewer  # noqa: E402

PART = 2048 * 512
SSID = "ios3ds-qemu"
PSK = "ios3ds-test-passphrase"
PKG = "ios3ds-perf-test"
VERSION = "1.0"
DEB = "%s_%s_iphoneos-arm.deb" % (PKG, VERSION)

SCRIPT = r'''#!/bin/bash
# Temporary Cydia/APT timing job inserted by emu/cydia_perf.py.
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
export HOME=/var/root
log() { echo "[cperf] $*" > /dev/console; }
for i in $(seq 1 240); do
    (echo > /dev/tcp/10.0.2.2/@PORT@) 2>/dev/null && break
    sleep 1
done
log "network ready after ${i}s"
OPTS="-o Dir::Etc::sourcelist=/private/etc/apt/sources.list.d/perf.list -o Dir::Etc::sourceparts=/nonexistent -o APT::Get::AllowUnauthenticated=true"
log "refresh start"
/usr/bin/apt-get update $OPTS > /dev/console 2>&1
log "refresh done rc=$?"
log "download start"
/usr/bin/apt-get -y --download-only install $OPTS $PKG > /dev/console 2>&1
log "download done rc=$?"
log "install start"
/usr/bin/dpkg -i /private/var/cache/apt/archives/%s > /dev/console 2>&1
log "install done rc=$?"
log "ALL DONE"
''' % DEB

PLIST = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
\t<key>Label</key>
\t<string>com.ios3ds.cydia-perf</string>
\t<key>ProgramArguments</key>
\t<array>
\t\t<string>/bin/bash</string>
\t\t<string>/usr/libexec/ios3ds-cydia-perf</string>
\t</array>
\t<key>RunAtLoad</key>
\t<true/>
\t<key>LaunchOnlyOnce</key>
\t<true/>
\t<key>UserName</key>
\t<string>root</string>
</dict>
</plist>
"""


def run(*args, **kw):
    return subprocess.run(args, capture_output=True, text=True, **kw)


def mtools(sd, *args):
    r = run(args[0], "-i", "%s@@%d" % (sd, PART), *args[1:])
    if r.returncode:
        raise SystemExit("mtools %s failed: %s%s" % (args[0], r.stdout, r.stderr))
    return r


def dummy_fw(path):
    rnd = random.Random(1)
    parts = {"stub_data": bytes(rnd.getrandbits(8) for _ in range(0x38)),
             "stub_code": bytes(rnd.getrandbits(8) for _ in range(0x316)),
             "database": bytes(rnd.getrandbits(8) for _ in range(0x1E8)),
             "main": bytes(rnd.getrandbits(8) for _ in range(0x9A00))}
    with open(path, "wb") as f:
        f.write(nwmfw.pack(parts))


def make_deb(outdir):
    """A tiny tweak-like package with 32 small files (dpkg fsync amplification)."""
    # build/ is on a filesystem without usable chmod(); dpkg-deb checks modes.
    tmp = tempfile.mkdtemp(prefix="ios3ds-deb-", dir="/tmp/opencode")
    root = os.path.join(tmp, "deb")
    ctl = os.path.join(root, "DEBIAN")
    data = os.path.join(root, "usr", "share", PKG)
    os.makedirs(ctl)
    os.makedirs(data)
    for d in (root, ctl, os.path.join(root, "usr"), data):
        os.chmod(d, 0o755)
    md5 = hashlib.md5()
    files = []
    for i in range(32):
        name = "resource%02d.txt" % i
        body = ("ios3ds perf payload %02d\n" % i).encode() * 16
        with open(os.path.join(data, name), "wb") as f:
            f.write(body)
        md5.update(body)
        files.append((name, len(body), hashlib.md5(body).hexdigest()))
    with open(os.path.join(ctl, "control"), "w") as f:
        f.write("Package: %s\nVersion: %s\nArchitecture: iphoneos-arm\n"
                "Maintainer: ios3ds test <test@example.invalid>\n"
                "Description: local timing-only package for emu/cydia_perf.py\n"
                " A small package built on the host; no third-party files.\n" % (PKG, VERSION))
    deb = os.path.join(outdir, DEB)
    # iOS 4's dpkg 1.18 cannot read zstd (the host dpkg-deb default).
    r = run("dpkg-deb", "-Zgzip", "-b", "--root-owner-group", root, deb)
    if r.returncode:
        # older dpkg-deb has no --root-owner-group
        r = run("dpkg-deb", "-Zgzip", "-b", root, deb)
    shutil.rmtree(tmp)
    if r.returncode:
        raise SystemExit("dpkg-deb failed: %s%s" % (r.stdout, r.stderr))
    packages = ("Package: %s\nVersion: %s\nArchitecture: iphoneos-arm\n"
                "Maintainer: ios3ds test <test@example.invalid>\n"
                "Filename: ./%s\nSize: %d\nMD5sum: %s\nSHA256: %s\n"
                "Description: local timing-only package\n"
                " Built by emu/cydia_perf.py.\n\n" %
                (PKG, VERSION, DEB, os.path.getsize(deb),
                 hashlib.md5(open(deb, "rb").read()).hexdigest(),
                 hashlib.sha256(open(deb, "rb").read()).hexdigest()))
    with open(os.path.join(outdir, "Packages"), "w") as f:
        f.write(packages)
    gz = gzip.compress(packages.encode(), mtime=0)
    with open(os.path.join(outdir, "Packages.gz"), "wb") as f:
        f.write(gz)
    # apt 0.7 asks for Release first; without it rc=100 even with
    # AllowUnauthenticated. Hashes are for the two index files only.
    rel = ["Origin: ios3ds-local-test", "Label: ios3ds-local-test",
           "Suite: stable", "Codename: stable", "Architectures: iphoneos-arm",
           "Components: main", "Description: local timing-only repository"]
    for field, fn in (("MD5Sum", lambda b: hashlib.md5(b).hexdigest()),
                      ("SHA256", lambda b: hashlib.sha256(b).hexdigest())):
        rel.append(field + ":")
        for name, body in (("Packages", packages.encode()), ("Packages.gz", gz)):
            rel.append(" %s %d %s" % (fn(body), len(body), name))
    with open(os.path.join(outdir, "Release"), "w") as f:
        f.write("\n".join(rel) + "\n")
    return deb


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        path = self.path.split("?", 1)[0]
        name = os.path.basename(path)
        full = os.path.join(self.server.repo, name)
        if name not in (DEB, "Packages", "Packages.gz", "Release") or not os.path.exists(full):
            self.send_error(404)
            return
        body = open(full, "rb").read()
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        print("[http] %s" % (fmt % args), flush=True)


def patch_image(sd, out, port, hfs_tool):
    """Put wifi, timing daemon, source list and repo files on the image."""
    hfs = os.path.join(out, "rootfs-perf.hfs")
    # extract the HFS+ volume from the FAT partition. -b is essential: mtype's
    # default text mode stops at the first ^Z and silently truncates the image.
    with open(hfs, "wb") as f:
        subprocess.run(["mtype", "-b", "-i", "%s@@%d" % (sd, PART), "::/ios3ds/rootfs.hfs"],
                       stdout=f, check=True)
    if os.path.getsize(hfs) != 1 << 30:
        raise SystemExit("HFS+ extraction truncated: %d bytes" % os.path.getsize(hfs))
    def h(*args):
        return run(hfs_tool, hfs, *args)
    script = os.path.join(out, "ios3ds-cydia-perf")
    plist = os.path.join(out, "com.ios3ds.cydia-perf.plist")
    with open(script, "w") as f:
        f.write(SCRIPT.replace("@PORT@", str(port)))
    with open(plist, "w") as f:
        f.write(PLIST)
    h("rm", "/usr/libexec/ios3ds-cydia-perf")
    h("add", script, "/usr/libexec/ios3ds-cydia-perf")
    h("chmod", "755", "/usr/libexec/ios3ds-cydia-perf")
    h("rm", "/System/Library/LaunchDaemons/com.ios3ds.cydia-perf.plist")
    h("add", plist, "/System/Library/LaunchDaemons/com.ios3ds.cydia-perf.plist")
    h("chmod", "644", "/System/Library/LaunchDaemons/com.ios3ds.cydia-perf.plist")
    source = os.path.join(out, "perf.list")
    with open(source, "w") as f:
        f.write("deb http://10.0.2.2:%d/repo ./\n" % port)
    h("rm", "/private/etc/apt/sources.list.d/perf.list")
    h("add", source, "/private/etc/apt/sources.list.d/perf.list")
    h("chmod", "644", "/private/etc/apt/sources.list.d/perf.list")
    h("mkdir", "/private/var/cache/apt/archives")
    h("rm", "/private/var/cache/apt/archives/" + DEB)
    h("add", os.path.join(out, DEB), "/private/var/cache/apt/archives/" + DEB)
    h("chmod", "644", "/private/var/cache/apt/archives/" + DEB)
    r = run(hfs_tool, hfs, "ls", "/")
    if r.returncode:
        raise SystemExit("hfsplus failed: %s" % r.stderr)
    # put the modified volume back
    mtools(sd, "mcopy", "-b", "-o", hfs, "::/ios3ds/rootfs.hfs")
    # FAT-side files: wifi.cfg, dummy firmware and timestamped console log
    cfg = os.path.join(out, "wifi.cfg")
    with open(cfg, "w") as f:
        f.write("ssid=%s\npsk=%s\nverbose=1\n" % (SSID, PSK))
    fw = os.path.join(out, "wifi-fw.bin")
    dummy_fw(fw)
    mtools(sd, "mcopy", "-b", "-o", cfg, "::/ios3ds/wifi.cfg")
    mtools(sd, "mcopy", "-b", "-o", fw, "::/ios3ds/wifi-fw.bin")
    boot = os.path.join(out, "boot.cfg")
    with open(boot, "wb") as f:
        subprocess.run(["mtype", "-b", "-i", "%s@@%d" % (sd, PART), "::/ios3ds/boot.cfg"],
                       stdout=f, check=True)
    with open(boot, "a") as f:
        f.write("\nlog_ts=1\nioprof=1\n")
    mtools(sd, "mcopy", "-b", "-o", boot, "::/ios3ds/boot.cfg")


def read_log(sd, out):
    path = os.path.join(out, "guest.log")
    with open(path, "wb") as f:
        subprocess.run(["mtype", "-b", "-i", "%s@@%d" % (sd, PART), "::/ios3ds/log.txt"],
                       stdout=f, check=False)
    return open(path, errors="replace").read()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--sd", default=os.path.join(ROOT, "build", "sd.img"))
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "cydia-perf"))
    ap.add_argument("--hfsplus", default=os.path.join(ROOT, "build", "tools", "hfsplus"))
    ap.add_argument("--qemu", default=os.environ.get("IOS3DS_QEMU") or viewer.QEMU)
    ap.add_argument("--firm", default=os.path.join(ROOT, "build", "boot", "ios3ds.firm"))
    ap.add_argument("--icount", default="2")
    ap.add_argument("--time", type=float, default=1500, help="wall-clock timeout in seconds")
    ap.add_argument("--port", type=int, default=0, help="host port (0 = random free high port)")
    ap.add_argument("--no-boot", action="store_true", help="prepare the image and server only")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    make_deb(a.out)
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
    srv.repo = a.out
    port = srv.server_address[1]
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    print("[cperf] HTTP repo on 127.0.0.1:%d (guest 10.0.2.2:%d)" % (port, port), flush=True)
    patch_image(a.sd, a.out, port, a.hfsplus)
    if a.no_boot:
        return 0
    sock = os.path.join(a.out, "qmp.sock")
    if os.path.exists(sock):
        os.remove(sock)
    extra = ("-nic user,model=ctr-wifi -global ctr-wifi.ssid=%s -global ctr-wifi.psk=%s"
             % (SSID, PSK))
    qlog = open(os.path.join(a.out, "qemu.txt"), "w")
    cmd = [a.qemu, "-M", "nintendo3ds", "-accel", "tcg,thread=single",
           "-kernel", a.firm,
           "-drive", "file=%s,if=sd,format=raw" % a.sd, "-display", "none", "-serial", "none",
           "-qmp", "unix:%s,server,nowait" % sock,
           "-icount", "shift=%s,sleep=off" % a.icount, "-rtc", "clock=vm"] + extra.split()
    print("[cperf] " + " ".join(cmd), flush=True)
    p = subprocess.Popen(cmd, stdout=qlog, stderr=subprocess.STDOUT, start_new_session=True)
    print("[cperf] qemu pid %d" % p.pid, flush=True)
    q = viewer.QMP(sock)
    t0 = time.time()
    done = False
    try:
        while time.time() - t0 < a.time:
            time.sleep(5)
            if p.poll() is not None:
                print("[cperf] QEMU exited early (%d)" % p.returncode, flush=True)
                break
            text = read_log(a.sd, a.out)
            if "[cperf] ALL DONE" in text:
                # let the log and the md0 write-back flush reach the SD card
                time.sleep(8)
                done = True
                break
    finally:
        try:
            q.cmd("quit")
        except Exception:
            pass
        try:
            p.wait(timeout=20)
        except subprocess.TimeoutExpired:
            os.kill(p.pid, 15)   # our own QEMU only
            p.wait(timeout=10)
        srv.shutdown()
    text = read_log(a.sd, a.out)
    with open(os.path.join(a.out, "report.txt"), "w") as f:
        for line in text.splitlines():
            if "[cperf]" in line or "[io] " in line or "[blk] rootfs" in line:
                f.write(line + "\n")
    print("----- [cperf] -----", flush=True)
    for line in text.splitlines():
        if "[cperf]" in line:
            print(line, flush=True)
    print("[cperf] %s (%.0f s wall; report %s)" %
          ("DONE" if done else "TIMEOUT", time.time() - t0, a.out), flush=True)
    return 0 if done else 1


if __name__ == "__main__":
    sys.exit(main())
