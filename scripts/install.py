#!/usr/bin/env python3
"""ios3ds installer: your own iOS 4.2.1 IPSW + this release -> a ready SD card.

  python3 install.py IPSW [SD]

  IPSW   iPod2,1_4.2.1_8C148_Restore.ipsw (the iPod touch 2G firmware, from Apple)
  SD     where to put the files: the root of your mounted SD card, or any folder
         (default: ./ios3ds-sd, copy its contents to the card yourself)

What it does (docs/INSTALL.md):
  1. decrypts the kernel, device tree and root filesystem from the IPSW
     (scripts/fwprep.py; the publicly documented keys for this one firmware
     come from api.ipsw.me, or --keys FILE)
  2. patches the kernel and builds the device tree, boot.cfg and the iOS disk
     image rootfs.hfs with our programs (scripts/mksd.py -> mkboot.py, mkrootfs.py)
  3. writes sd:/ios3ds/, sd:/3ds/ios3ds.3dsx and sd:/luma/payloads/ios3ds.firm

Needs Python 3.8+ with the 'cryptography' package (or the openssl command) and
~4 GB of free disk space while it works. No cross compilers: the 3DS and iOS
binaries come prebuilt in this release. On Linux x86_64 the HFS+ tool is
prebuilt too; elsewhere it is compiled from src/xpwn-hfsplus (needs cc + zlib).
Nothing here talks to the 3DS or writes anything but files in SD and --work.
"""
import argparse
import json
import os
import platform
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE) if os.path.basename(HERE) == "scripts" else HERE
SCRIPTS = os.path.join(ROOT, "scripts")
sys.path.insert(0, SCRIPTS)

TARGET_IPSW = "iPod2,1_4.2.1_8C148_Restore.ipsw"
GiB = 1 << 30


def say(msg=""):
    print(msg, flush=True)


def step(n, msg):
    say("\n== %d. %s" % (n, msg))


def die(msg):
    say("\nERROR: " + msg)
    sys.exit(1)


def free_bytes(path):
    p = os.path.abspath(path)
    while not os.path.exists(p):
        p = os.path.dirname(p)
    return shutil.disk_usage(p).free


def check_python():
    if sys.version_info < (3, 8):
        die("Python 3.8 or newer is needed (this is %s)" % sys.version.split()[0])
    try:
        import cryptography  # noqa: F401
        return "cryptography"
    except ImportError:
        if shutil.which("openssl"):
            say("note: the Python 'cryptography' package is missing; using the openssl command\n"
                "      instead, which is much slower (pip install cryptography, or\n"
                "      sudo apt install python3-cryptography)")
            return "openssl"
        die("install the Python 'cryptography' package: pip install cryptography\n"
            "       (Debian/Ubuntu: sudo apt install python3-cryptography)")


def hfsplus_tool(prebuilt, work):
    """xpwn's hfsplus: the static Linux binary from the release, else compile it."""
    def works(p):
        try:
            r = subprocess.run([p], capture_output=True, text=True)
        except OSError:
            return False
        return "usage" in (r.stdout + r.stderr)
    arch = platform.machine().lower()
    cand = os.path.join(prebuilt, "hfsplus-linux-%s" % arch)
    if platform.system() == "Linux" and os.path.exists(cand):
        try:
            os.chmod(cand, 0o755)       # zip tools often drop the x bit
        except OSError:
            pass
        if works(cand):
            return cand
    built = os.path.join(work, "hfsplus")
    if os.path.exists(built) and works(built):
        return built
    src = os.path.join(ROOT, "src", "xpwn-hfsplus")
    cc = os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if not cc or not os.path.isdir(src):
        die("no HFS+ tool for this system (%s %s) and no C compiler to build it.\n"
            "       Linux: sudo apt install build-essential zlib1g-dev; macOS: xcode-select --install;\n"
            "       Windows: run this installer inside WSL (Ubuntu)." % (platform.system(), arch))
    say("building the HFS+ tool from src/xpwn-hfsplus with %s ..." % cc)
    os.makedirs(work, exist_ok=True)
    import glob
    files = sorted(glob.glob(os.path.join(src, "hfs", "*.c"))) + \
        [os.path.join(src, "common", f) for f in ("abstractfile.c", "base64.c")]
    r = subprocess.run([cc, "-O2", "-w", "-I" + os.path.join(src, "includes")] + files +
                       ["-lz", "-o", built], capture_output=True, text=True)
    if r.returncode or not works(built):
        die("could not build the HFS+ tool:\n%s\n       (needs zlib headers: sudo apt install zlib1g-dev)"
            % (r.stderr.strip()[-2000:]))
    return built


def fwprep(ipsw, staging, keys, force):
    man = os.path.join(staging, "manifest.json")
    if os.path.exists(man) and os.path.exists(os.path.join(staging, "rootfs.hfs")):
        try:
            src = json.load(open(man))["source"]
            if src.get("ipsw") == os.path.basename(ipsw) and src.get("build") == "8C148":
                say("already extracted in %s (delete it to redo)" % staging)
                return
        except (ValueError, KeyError):
            pass
    cmd = [sys.executable, os.path.join(SCRIPTS, "fwprep.py"), ipsw, "-o", staging]
    if keys:
        cmd += ["--keys", keys]
    if force:
        cmd += ["--force"]
    if subprocess.run(cmd).returncode:
        die("extracting the IPSW failed (see the [fwprep] lines above)")


def build_tree(staging, tree, prebuilt, hfsplus, cydia=True, cydia_cache=None):
    import mksd
    args = mksd.parser().parse_args([])
    args.staging = staging
    args.out = tree
    args.firm = os.path.join(prebuilt, "ios3ds.firm")
    args.launcher = os.path.join(prebuilt, "ios3ds.3dsx")
    args.prebuilt = prebuilt
    args.hfsplus = hfsplus
    args.rootfs_add = os.path.join(prebuilt, "rootfs-add")
    args.no_cydia = not cydia
    args.cydia_cache = cydia_cache
    mksd.build_tree(args)


def copy_file(src, dst):
    size = os.path.getsize(src)
    tmp = dst + ".part"
    done, t0, last = 0, time.time(), 0
    with open(src, "rb") as fi, open(tmp, "wb") as fo:
        while True:
            b = fi.read(8 << 20)
            if not b:
                break
            fo.write(b)
            done += len(b)
            if size > 64 << 20 and time.time() - last > 2:
                last = time.time()
                print("\r   %s: %3d%% (%d/%d MiB)" % (os.path.basename(dst), 100 * done // size,
                      done >> 20, size >> 20), end="", flush=True)
        fo.flush()
        os.fsync(fo.fileno())
    if size > 64 << 20:
        print("\r   %s: done (%d MiB, %.0f s)          " % (os.path.basename(dst), size >> 20,
              time.time() - t0), flush=True)
    os.replace(tmp, dst)


def deploy(tree, sd):
    """Copy the card tree into sd, leaving everything else on the card alone."""
    for d, _, fs in os.walk(tree):
        rel = os.path.relpath(d, tree)
        os.makedirs(os.path.join(sd, rel), exist_ok=True)
        for fn in sorted(fs):
            src, dst = os.path.join(d, fn), os.path.normpath(os.path.join(sd, rel, fn))
            if fn == "boot.cfg" and os.path.exists(dst):
                old = open(dst, "rb").read()
                if old != open(src, "rb").read():
                    shutil.copyfile(dst, dst + ".old")
                    say("   kept your previous boot.cfg as ios3ds/boot.cfg.old")
            copy_file(src, dst)
            say("   %s" % os.path.relpath(dst, sd))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ipsw", nargs="?", help="iPod2,1_4.2.1_8C148_Restore.ipsw")
    ap.add_argument("sd", nargs="?", default="ios3ds-sd",
                    help="SD card root (mounted) or output folder (default: ./ios3ds-sd)")
    ap.add_argument("--work", default="ios3ds-work", help="scratch folder (default: ./ios3ds-work)")
    ap.add_argument("--keep-work", action="store_true", help="keep the scratch folder (faster re-runs)")
    ap.add_argument("--keys", help="firmware keys JSON (ipsw.me v4 format) instead of downloading")
    ap.add_argument("--staging", help="use an already extracted fwprep folder instead of the IPSW")
    ap.add_argument("--force", action="store_true", help="accept an IPSW other than iPod2,1 8C148")
    ap.add_argument("--no-cydia", action="store_true",
                    help="don't download and pre-install Cydia (docs/cydia.md)")
    ap.add_argument("--prebuilt", default=os.path.join(ROOT, "prebuilt"), help=argparse.SUPPRESS)
    ap.add_argument("--check", action="store_true",
                    help="only check this release and the tools on this PC, then exit")
    a = ap.parse_args()

    say("ios3ds installer (%s)" % open(os.path.join(a.prebuilt, "VERSION")).read().strip()
        if os.path.exists(os.path.join(a.prebuilt, "VERSION")) else "ios3ds installer")
    need = ["ios3ds.firm", "ios3ds.3dsx", "shim.bin", "gpu.bin", "rootfs-add"]
    missing = [n for n in need if not os.path.exists(os.path.join(a.prebuilt, n))]
    if missing:
        die("this release is incomplete, missing prebuilt/%s. Unpack the whole zip." % ", ".join(missing))

    step(1, "Checking this PC")
    crypto = check_python()
    hfs = hfsplus_tool(a.prebuilt, a.work)
    say("Python %s, %s, HFS+ tool %s" % (platform.python_version(), crypto, os.path.relpath(hfs)))
    if a.check:
        import mkboot  # noqa: F401  (imports kernel/, fwlib: catches a broken unpack)
        import mkrootfs  # noqa: F401
        import cydia  # noqa: F401
        import certs  # noqa: F401  (needs sqlite3)
        import fwprep as _fwprep_check # noqa: F401
        say("OK: everything needed is here")
        return

    if a.staging and a.ipsw and not a.ipsw.lower().endswith(".ipsw"):
        a.ipsw, a.sd = None, a.ipsw         # install.py --staging DIR SD
    if not a.ipsw and not a.staging:
        ap.print_usage()
        die("give the path of your %s" % TARGET_IPSW)
    if a.ipsw and not os.path.isfile(a.ipsw):
        die("no such file: %s" % a.ipsw)
    sd = os.path.abspath(a.sd)
    work = os.path.abspath(a.work)
    if sd == work or sd.startswith(work + os.sep) or work.startswith(sd + os.sep):
        die("the SD folder and --work must be separate folders")
    if os.path.exists(os.path.join(sd, "ios3ds", "luma.firm")):
        die("%s has ios3ds/luma.firm: a launch from the Homebrew Launcher was not finished.\n"
            "       Start ios3ds once (it restores Luma by itself), or use the launcher's\n"
            "       'Y: restore Luma', or rename ios3ds/luma.firm back to boot.firm on the PC." % sd)
    if free_bytes(work) < 4 * GiB:
        die("need about 4 GB free for %s (have %.1f GB)" % (work, free_bytes(work) / GiB))
    if free_bytes(sd) < 2 * GiB:
        die("need about 2 GB free on %s (have %.1f GB)" % (sd, free_bytes(sd) / GiB))

    staging = os.path.abspath(a.staging) if a.staging else os.path.join(work, "staging")
    if not a.staging:
        step(2, "Extracting your IPSW (kernel, device tree, root filesystem; a few minutes)")
        fwprep(a.ipsw, staging, a.keys, a.force)
    else:
        step(2, "Using the extracted firmware in %s" % staging)

    step(3, "Patching the kernel, building the iOS disk image (a few minutes)")
    tree = os.path.join(work, "sd")
    build_tree(staging, tree, a.prebuilt, hfs, cydia=not a.no_cydia,
               cydia_cache=os.path.join(work, "cydia"))

    step(4, "Writing the SD card files to %s" % sd)
    os.makedirs(os.path.dirname(sd), exist_ok=True)
    if not os.path.exists(sd) and os.stat(os.path.dirname(sd)).st_dev == os.stat(work).st_dev:
        os.rename(tree, sd)             # a new folder on the same disk: just move it
    else:
        os.makedirs(sd, exist_ok=True)
        deploy(tree, sd)
    if not a.keep_work:
        shutil.rmtree(tree, ignore_errors=True)
        if not a.staging:
            shutil.rmtree(staging, ignore_errors=True)
        for leftover in ("hfsplus",):
            if os.path.exists(os.path.join(work, leftover)):
                os.remove(os.path.join(work, leftover))
        shutil.rmtree(os.path.join(work, "cydia"), ignore_errors=True)   # downloaded .debs
        try:
            os.rmdir(work)
        except OSError:
            pass
    say("\nDone. On the SD card (%s):" % sd)
    say("  3ds/ios3ds.3dsx              start this from the Homebrew Launcher")
    say("  luma/payloads/ios3ds.firm    or hold START while powering on (Luma's chainloader)")
    say("  ios3ds/                      iOS itself (~1 GB) and its settings (boot.cfg)")
    if os.path.basename(sd) == "ios3ds-sd":
        say("\nCopy the CONTENTS of %s to the root of your SD card." % sd)
    say("Then see INSTALL.md: 'First boot'.")


if __name__ == "__main__":
    main()
