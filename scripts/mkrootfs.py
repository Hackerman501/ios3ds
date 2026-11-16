#!/usr/bin/env python3
"""Prepare the root filesystem image for ios3ds: copy the staged 4.2.1 rootfs.hfs
and apply rootfs/recipe.json with xpwn's hfsplus tool (build/tools/hfsplus,
built by scripts/build-hfsplus.sh). Output stays in build/ (never committed).

  scripts/mkrootfs.py [--staging build/staging] [--out build/sd/ios3ds/rootfs.hfs]
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HFS = os.path.join(ROOT, "build", "tools", "hfsplus")
LD = "/System/Library/LaunchDaemons"
LD_OFF = "/System/Library/LaunchDaemons-ios3ds-disabled"


def hfs(img, *args, check=True):
    r = subprocess.run([HFS, img] + list(args), capture_output=True, text=True)
    if check and r.returncode:
        raise SystemExit("hfsplus %s failed: %s" % (" ".join(args), r.stderr.strip() or r.stdout.strip()))
    return r.stdout


def hfs_cat(img, path):
    r = subprocess.run([HFS, img, "cat", path], capture_output=True)
    if r.returncode:
        raise SystemExit("hfsplus cat %s failed" % path)
    return r.stdout


def exists(img, path):
    d, _, name = path.rstrip("/").rpartition("/")
    out = hfs(img, "ls", d or "/", check=False)
    return any(l.split()[-1:] == [name] for l in out.splitlines() if l.strip())


def build(staging, out, recipe_path=None, verbose=True):
    if not os.path.exists(HFS):
        subprocess.run([os.path.join(ROOT, "scripts", "build-hfsplus.sh")], check=True)
    recipe = json.load(open(recipe_path or os.path.join(ROOT, "rootfs", "recipe.json")))
    src = os.path.join(staging, "rootfs.hfs")
    if os.path.exists(out):
        os.remove(out)
    shutil.copyfile(src, out)
    for path in recipe.get("remove", []):
        # frees blocks, which 'grow' needs to enlarge the allocation bitmap
        hfs(out, "rm", path)
        if verbose:
            print("[mkrootfs] removed %s" % path)
    if recipe.get("grow_bytes"):
        # Apple's image has 0 bytes free: launchd, syslogd & co need to write
        res = hfs(out, "grow", str(recipe["grow_bytes"]))
        if "grew volume" not in res:
            raise SystemExit("hfsplus grow failed: " + res.strip())
        if verbose:
            print("[mkrootfs] grew volume to %d MiB" % (os.path.getsize(out) >> 20))
    with tempfile.TemporaryDirectory() as td:
        for path, text in recipe.get("write", {}).items():
            tmp = os.path.join(td, "f")
            with open(tmp, "w") as f:
                f.write(text)
            if exists(out, path):
                hfs(out, "rm", path)
            hfs(out, "add", tmp, path)
            if verbose:
                print("[mkrootfs] wrote %s" % path)
        for local, path in recipe.get("add", {}).items():
            if exists(out, path):
                hfs(out, "rm", path)
            hfs(out, "add", os.path.join(ROOT, local), path)
            if verbose:
                print("[mkrootfs] added %s -> %s" % (local, path))
        for path, keys in recipe.get("plist_set", {}).items():
            import plistlib
            tmp = os.path.join(td, "p.plist")
            with open(tmp, "wb") as f:
                f.write(hfs_cat(out, path))
            with open(tmp, "rb") as f:
                pl = plistlib.load(f)
            pl.update(keys)
            with open(tmp, "wb") as f:
                plistlib.dump(pl, f, fmt=plistlib.FMT_XML)
            hfs(out, "rm", path)
            hfs(out, "add", tmp, path)
            if verbose:
                print("[mkrootfs] %s: set %s" % (path, ", ".join(keys)))
        by_path = {}
        for fp in recipe.get("file_patches", []):
            by_path.setdefault(fp["path"], []).append(fp)
        for path, fps in by_path.items():
            tmp = os.path.join(td, "patched")
            with open(tmp, "wb") as f:
                f.write(hfs_cat(out, path))
            with open(tmp, "r+b") as f:
                for fp in fps:
                    off = int(fp["offset"], 0)
                    orig, new = bytes.fromhex(fp["orig"]), bytes.fromhex(fp["new"])
                    f.seek(off)
                    if f.read(len(orig)) != orig:
                        raise SystemExit("file patch %s @%#x: original bytes differ" % (path, off))
                    f.seek(off)
                    f.write(new)
                    if verbose:
                        print("[mkrootfs] patched %s @%#x: %s" % (path, off, fp.get("why", "")))
            hfs(out, "rm", path)
            hfs(out, "add", tmp, path)
            hfs(out, "chmod", "0644", path)
        for path, mode in recipe.get("chmod", {}).items():
            hfs(out, "chmod", mode, path)
    if recipe.get("disable_daemons"):
        hfs(out, "mkdir", LD_OFF, check=False)
        n = 0
        for name in recipe["disable_daemons"]:
            if exists(out, LD + "/" + name):
                hfs(out, "mv", LD + "/" + name, LD_OFF + "/" + name)
                n += 1
            elif verbose:
                print("[mkrootfs] (daemon %s not present)" % name)
        if verbose:
            print("[mkrootfs] disabled %d launch daemons" % n)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--staging", default=os.path.join(ROOT, "build", "staging"))
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "sd", "ios3ds", "rootfs.hfs"))
    a = ap.parse_args()
    build(a.staging, a.out)


if __name__ == "__main__":
    main()
