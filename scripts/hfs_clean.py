#!/usr/bin/env python3
"""Report the HFS+ clean-unmount flag of ios3ds's root disk image.

  python3 scripts/hfs_clean.py build/sd/ios3ds/rootfs.hfs
  python3 scripts/hfs_clean.py --sd build/sd.img

The volume header lives at byte 1024 of the image: signature ('H+'/'HX'),
version, then the big-endian 32-bit attributes. Bit 8 (0x0100) is
kHFSVolumeUnmountedMask: XNU sets it on a clean unmount and clears it when the
volume is mounted read-write. Our image has no journal and launchctl cannot
run fsck on the mounted md0 (EBUSY), so a clear bit means the last iOS run was
cut short. Exit status: 0 clean, 1 dirty, 2 unreadable.
"""
import argparse
import struct
import subprocess
import sys

VH_OFF = 1024
UNMOUNTED = 1 << 8


def attrs_from_header(data):
    """Return the volume attributes from at least VH_OFF+8 bytes, or None."""
    if len(data) < VH_OFF + 8:
        return None
    vh = data[VH_OFF:VH_OFF + 8]
    if vh[:2] not in (b"H+", b"HX"):
        return None
    return struct.unpack(">I", vh[4:8])[0]


def attrs_from_file(path):
    with open(path, "rb") as f:
        return attrs_from_header(f.read(VH_OFF + 8))


def attrs_from_sd_image(path, offset=1024 * 1024, mcopy="mcopy"):
    """Read rootfs.hfs's header out of an SD card image (FAT at `offset`)."""
    p = subprocess.Popen([mcopy, "-i", "%s@@%d" % (path, offset),
                          "::/ios3ds/rootfs.hfs", "-"],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    head = p.stdout.read(VH_OFF + 8)
    p.stdout.close()
    p.wait()
    return attrs_from_header(head)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("image", nargs="?", help="raw rootfs.hfs file")
    ap.add_argument("--sd", help="raw SD image; read ios3ds/rootfs.hfs from its FAT partition")
    a = ap.parse_args()
    if bool(a.sd) == bool(a.image):
        ap.error("give either an .hfs file or --sd IMAGE")
    if a.sd:
        attrs = attrs_from_sd_image(a.sd)
        what = a.sd + " ::/ios3ds/rootfs.hfs"
    else:
        attrs = attrs_from_file(a.image)
        what = a.image
    if attrs is None:
        print("%s: no HFS+ volume header" % what)
        return 2
    clean = bool(attrs & UNMOUNTED)
    print("%s: attributes 0x%08x: %s" % (what, attrs,
          "cleanly unmounted" if clean else "NOT cleanly unmounted"))
    return 0 if clean else 1


if __name__ == "__main__":
    sys.exit(main())
