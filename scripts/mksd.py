#!/usr/bin/env python3
"""Build the SD card contents for ios3ds, and optionally a raw SD image for QEMU.

  scripts/mksd.py [--image build/sd.img] [--size 2G] [--staging build/staging]

Always writes the card tree to build/sd/ (--out; copy its contents to the root
of the real SD card). scripts/install.py calls this with a release's prebuilt
files (--firm, --launcher, --prebuilt, --rootfs-add, --hfsplus) instead of build/:
  3ds/ios3ds.3dsx             Homebrew Launcher entry (if built): swaps boot.firm, reboots
  ios3ds/ios3ds.firm          the payload again, for the launcher
  gm9/scripts/ios3ds-wifi-fw.gm9  GodMode9 script: NWM code -> ios3ds/nwm.code (Wi-Fi)
  luma/payloads/ios3ds.firm   the payload (hold START at boot for Luma's menu,
                              or rename to x_ios3ds.firm to boot it with X)
  ios3ds/boot.cfg             loader configuration (editable: cmdline=...)
  ios3ds/kernel.bin           flat kernel image   (only if the staging dir has a kernel)
  ios3ds/devtree.bin          generated device tree

With --image, also creates a partitioned FAT32 image (MBR, partition at 1 MiB)
using sfdisk + mkfs.fat + mtools, no root needed.
"""
import argparse
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))


def run(cmd, **kw):
    subprocess.run(cmd, check=True, **kw)


def build_tree(args):
    tree = args.out
    fresh = os.environ.get("IOS3DS_FRESH", "").lower() in ("1", "true", "yes", "on") or \
            os.environ.get("FRESH", "").lower() in ("1", "true", "yes", "on")
    carry_from = getattr(args, "carry_from", None)
    if not carry_from:
        carry_from = os.environ.get("IOS3DS_CARRY_FROM") or os.environ.get("CARRY_FROM")
    if carry_from:
        carry_from = os.path.abspath(carry_from)
        if not os.path.isfile(carry_from):
            sys.exit("[mksd] CARRY_FROM image not found: %s" % carry_from)
    saved_from = None
    if carry_from:
        # allow CARRY_FROM=build/sd/ios3ds/rootfs.hfs (the previous build):
        # move it out of the tree that is about to be replaced, no extra copy
        tree_abs = os.path.abspath(tree)
        if carry_from.startswith(tree_abs + os.sep):
            saved_from = tree_abs + ".carry-src.hfs"
            if os.path.exists(saved_from):
                os.remove(saved_from)
            shutil.move(carry_from, saved_from)
            carry_from = saved_from
    if os.path.isdir(tree):
        shutil.rmtree(tree)
    os.makedirs(os.path.join(tree, "luma", "payloads"))
    os.makedirs(os.path.join(tree, "ios3ds"))
    firm = args.firm
    if not os.path.exists(firm):
        sys.exit("build the payload first: make -C boot")
    shutil.copy(firm, os.path.join(tree, "luma", "payloads", "ios3ds.firm"))
    # for the Homebrew Launcher route (tools/ios3ds-launcher, docs/real-hardware.md)
    shutil.copy(firm, os.path.join(tree, "ios3ds", "ios3ds.firm"))
    launcher = args.launcher
    if os.path.exists(launcher):
        os.makedirs(os.path.join(tree, "3ds"), exist_ok=True)
        shutil.copy(launcher, os.path.join(tree, "3ds", "ios3ds.3dsx"))
    # GodMode9 script that copies the console's own NWM code (Wi-Fi firmware
    # source) to sd:/ios3ds/nwm.code, read-only on NAND (docs/wifi.md section 3)
    gm9 = os.path.join(ROOT, "tools", "ios3ds-wifi", "ios3ds-wifi-fw.gm9")
    if os.path.exists(gm9):
        os.makedirs(os.path.join(tree, "gm9", "scripts"), exist_ok=True)
        shutil.copy(gm9, os.path.join(tree, "gm9", "scripts", "ios3ds-wifi-fw.gm9"))
    kernel = os.path.join(args.staging, "kernelcache.macho")
    if os.path.exists(kernel) and not args.no_kernel:
        import mkboot  # noqa: E402
        mkboot.build(args.staging, os.path.join(tree, "ios3ds"), cmdline=args.cmdline,
                     patches=not args.no_patches, prebuilt=args.prebuilt,
                     hfsplus=args.hfsplus, rootfs_add=args.rootfs_add,
                     cydia=not args.no_cydia, cydia_cache=args.cydia_cache,
                     tlsfix=not args.no_tlsfix, sandbox_open=args.sandbox_open,
                     music=args.music, test_song=not args.no_test_song,
                     version=args.ios)
        new_img = os.path.join(tree, "ios3ds", "rootfs.hfs")
        if carry_from and os.path.isfile(carry_from) and os.path.isfile(new_img) and not fresh:
            sys.path.insert(0, os.path.join(ROOT, "scripts"))
            import carryover  # noqa: E402
            hfs = args.hfsplus
            if not hfs:
                hfs = os.path.join(ROOT, "build", "tools", "hfsplus")
            try:
                result = carryover.carry_over(carry_from, new_img, hfs, backup=False, verbose=True)
                print("[mksd] carry-over result: %s" % result)
            except Exception as e:
                print("[mksd] warn: carry-over failed: %s" % e)
        elif carry_from:
            print("[mksd] no new rootfs; skipping carry-over from %s" % carry_from)
    else:
        print("[mksd] no staged kernel (or --no-kernel): SD gets the payload only")
    if saved_from:
        for extra in (saved_from, saved_from + ".bak"):
            if os.path.exists(extra):
                os.remove(extra)
    return tree


def build_image(tree, path, size):
    if os.path.exists(path):
        os.remove(path)
    with open(path, "wb") as f:
        f.truncate(size)
    run(["sfdisk", "--quiet", path], input=b"label: dos\nstart=2048, type=c\n")
    part_off = 2048 * 512
    run(["mkfs.fat", "-F", "32", "-n", "IOS3DS", "--offset", "2048", path,
         str((size - part_off) // 1024)], stdout=subprocess.DEVNULL)
    img = "%s@@%d" % (path, part_off)
    for dirpath, dirnames, filenames in os.walk(tree):
        rel = os.path.relpath(dirpath, tree)
        for d in dirnames:
            run(["mmd", "-i", img, "::/" + os.path.join(rel, d).lstrip("./")])
        for fn in filenames:
            dst = "::/" + os.path.join(rel, fn).lstrip("./")
            run(["mcopy", "-i", img, os.path.join(dirpath, fn), dst])
    print("[mksd] image %s (%d MiB)" % (path, size >> 20))


def parser():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", help="also write a raw SD image for QEMU")
    ap.add_argument("--size", default="2G")
    ap.add_argument("--staging", default=os.path.join(ROOT, "build", "staging"))
    ap.add_argument("--cmdline", default=None, help="override the XNU boot-args")
    ap.add_argument("--no-kernel", action="store_true")
    ap.add_argument("--no-patches", action="store_true", help="boot the unpatched kernel")
    # inputs/outputs; the defaults are the repo build, the installer passes the release's files
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "sd"), help="card tree (replaced)")
    ap.add_argument("--firm", default=os.path.join(ROOT, "build", "boot", "ios3ds.firm"))
    ap.add_argument("--launcher", default=os.path.join(ROOT, "build", "launcher", "ios3ds.3dsx"))
    ap.add_argument("--prebuilt", help="dir with a prebuilt shim.bin + gpu.bin (else compiled)")
    ap.add_argument("--hfsplus", help="xpwn hfsplus tool (else build/tools/hfsplus)")
    ap.add_argument("--rootfs-add", help="dir standing in for build/rootfs-add/")
    ap.add_argument("--no-cydia", action="store_true",
                    help="no Cydia in the rootfs (else downloaded per rootfs/cydia.json, docs/cydia.md)")
    ap.add_argument("--no-tlsfix", action="store_true",
                    help="no TLSFix (TLS 1.2/1.3 for iOS) in the rootfs (docs/certificates.md)")
    ap.add_argument("--sandbox-open", action="store_true",
                    help="relax the sandbox so TLSFix reaches Safari/App Store apps (opt-in; "
                         "Safari OOMs on 128 MB, docs/cydia.md)")
    ap.add_argument("--cydia-cache", help="where to keep the Cydia .debs (default build/cydia)")
    ap.add_argument("--music", help="folder with your audio files (.mp3/.m4a/.wav/.aif) for the Music app "
                    "(docs/audio.md section 10)")
    ap.add_argument("--no-test-song", action="store_true", help="leave the generated test song out of the Music app")
    ap.add_argument("--carry-from", help="old rootfs.hfs to carry user data from (default: IOS3DS_CARRY_FROM env; none = no carry-over)")
    ap.add_argument("--ios", default="4.2.1", help="iOS version to build for (default 4.2.1, scripts/versions.py)")
    return ap


def main():
    a = parser().parse_args()
    mult = {"M": 1 << 20, "G": 1 << 30}
    size = int(a.size[:-1]) * mult[a.size[-1].upper()] if a.size[-1].isalpha() else int(a.size)
    tree = build_tree(a)
    print("[mksd] card tree: %s" % tree)
    if a.image:
        build_image(tree, a.image, size)


if __name__ == "__main__":
    main()
