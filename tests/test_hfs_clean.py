#!/usr/bin/env python3
"""Host tests for the HFS+ clean-unmount flag reader (re-log 86)."""
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "scripts"))
import hfs_clean  # noqa: E402


def header(sig=b"HX", attrs=0):
    return b"\0" * hfs_clean.VH_OFF + sig + struct.pack(">HI", 5, attrs)


class HfsCleanTest(unittest.TestCase):
    def test_header(self):
        self.assertEqual(hfs_clean.attrs_from_header(header(attrs=0x80000100)), 0x80000100)
        self.assertEqual(hfs_clean.attrs_from_header(header(sig=b"H+", attrs=0x12345678)), 0x12345678)

    def test_bad_header(self):
        for data in (b"", b"\0" * 100, header(sig=b"BD"), header()[:1024 + 7]):
            self.assertIsNone(hfs_clean.attrs_from_header(data))

    def test_flag(self):
        clean = hfs_clean.attrs_from_header(header(attrs=0x80000100))
        dirty = hfs_clean.attrs_from_header(header(attrs=0x80000000))
        self.assertTrue(clean & hfs_clean.UNMOUNTED)
        self.assertFalse(dirty & hfs_clean.UNMOUNTED)

    def test_file(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "rootfs.hfs")
            with open(p, "wb") as f:
                f.write(header(attrs=0x80000000))
            self.assertEqual(hfs_clean.attrs_from_file(p), 0x80000000)


if __name__ == "__main__":
    unittest.main()
