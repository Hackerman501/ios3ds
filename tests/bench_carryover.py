#!/usr/bin/env python3
"""Time scripts/carryover.py with 1000 small files + 50 x 300 KiB in DCIM.

  python3 tests/bench_carryover.py

Only needs build/tools/hfsplus and mkfs.hfsplus (the same tools as the unit
tests).  The fixture setup is not timed; only carry_over() is.
"""
import os
import shutil
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "scripts"))

from test_carryover import HFS, MKFS, hfs, make_image, set_folder_owner  # noqa: E402
import carryover  # noqa: E402

DIRS = ("/private", "/private/var", "/private/var/mobile", "/private/var/mobile/Library",
        "/private/var/mobile/Media", "/private/var/mobile/Media/DCIM")


def main():
    if not os.path.exists(HFS) or not MKFS:
        sys.exit("need build/tools/hfsplus and mkfs.hfsplus")
    td = tempfile.mkdtemp(prefix="ios3ds-carry-bench-")
    try:
        old, new = os.path.join(td, "old.hfs"), os.path.join(td, "new.hfs")
        make_image(old, 96 << 20)
        make_image(new, 96 << 20)
        for img in (old, new):
            for d in DIRS:
                hfs(img, "mkdir", d, check=False)
            set_folder_owner(img, "mobile", 501, 501)
            set_folder_owner(img, "Library", 501, 501)
            set_folder_owner(img, "Media", 501, 501)
            set_folder_owner(img, "DCIM", 501, 501)
        src = os.path.join(td, "DCIM")
        os.makedirs(src)
        for i in range(1000):
            with open(os.path.join(src, "IMG_%04d.JPG" % i), "wb") as f:
                f.write(b"x" * 37)
        for i in range(50):
            with open(os.path.join(src, "BIG_%04d.JPG" % i), "wb") as f:
                f.truncate(300 * 1024)
        hfs(old, "addall", src, "/private/var/mobile/Media/DCIM/")
        t0 = time.monotonic()
        result = carryover.carry_over(old, new, HFS, verbose=False)
        elapsed = time.monotonic() - t0
        print("carry_over(): %s in %.1f s (1000 small + 50 x 300 KiB files)" %
              (result, elapsed))
        print("new image free: %d bytes" % carryover.free_space(HFS, new))
        return 0 if result == "carried" and elapsed < 120 else 1
    finally:
        shutil.rmtree(td, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
