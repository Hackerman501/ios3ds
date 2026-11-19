"""Tests for tools/ios3ds-wifi/nwmfw.py on synthetic NWM .code files (random
bytes, no Nintendo code): both literal-pool layouts, BLZ, error paths.
SPDX-License-Identifier: GPL-3.0-or-later
"""
import os
import random
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import nwmfw  # noqa: E402

BASE = 0x00100000
NWM_HOST = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..",
                        "build", "wifi-tests", "nwm_host")


def blz_compress(plain, prefix):
    """Minimal greedy BLZ encoder (inverse of nwmfw.blz_decompress): the first
    `prefix` bytes stay uncompressed."""
    n = len(plain)
    toks, pos = [], n
    while pos > prefix:
        best_l, best_d = 0, 0
        for d in range(3, min(0x1002, n - pos, 64) + 1):     # short window: fast enough for tests
            ln = 0
            while ln < 18 and pos - ln > prefix and plain[pos - 1 - ln] == plain[pos - 1 - ln + d]:
                ln += 1
            if ln > best_l:
                best_l, best_d = ln, d
        if best_l >= 3:
            toks.append(("c", best_l, best_d))
            pos -= best_l
        else:
            toks.append(("l", plain[pos - 1]))
            pos -= 1
    rev = []
    for g in range(0, len(toks), 8):
        group = toks[g:g + 8]
        flag = 0
        body = []
        for i, t in enumerate(group):
            if t[0] == "c":
                flag |= 0x80 >> i
                seg = (t[1] - 3) << 12 | (t[2] - 3)
                body += [seg >> 8, seg & 0xFF]          # hi read first (higher address)
            else:
                body.append(t[1])
        rev += [flag] + body
    region = bytes(reversed(rev))
    comp = bytearray(plain[:prefix]) + region
    comp += struct.pack("<II", 8 << 24 | (len(region) + 8), 0)
    struct.pack_into("<I", comp, len(comp) - 4, n - len(comp))
    return bytes(comp)


def make_code(layout="newer", rnd=None, bad_stub=False):
    rnd = rnd or random.Random(1)
    blobs = {"t1": 0x1B1B, "t4": 0xA5EB, "t5": 0x7A2E, "db": 0x1E8,
             "code": 0x316 + (1 if bad_stub else 0), "data": 0x38}
    c = bytearray(rnd.getrandbits(8) for _ in range(0x400))
    addr = {}
    for k, ln in blobs.items():
        s = BASE + len(c)
        c += bytes(rnd.getrandbits(8) for _ in range(ln))
        addr[k] = (s, s + ln)
        c += b"\0" * (-len(c) % 4)
    w = lambda *v: struct.pack("<%dI" % len(v), *v)  # noqa: E731
    if layout == "newer":
        pool = w(0x11, 0x22, 0x33, 0x44, 0x55, 0x00524C00, 0x3ED,
                 addr["t1"][1], addr["t1"][0], addr["t4"][1], addr["t4"][0], addr["t5"][1], addr["t5"][0],
                 addr["db"][1], addr["db"][0], 0x0053FE18,
                 addr["code"][1], addr["code"][0], 0x00527000,
                 addr["data"][1], addr["data"][0], 0x1, 0x2, 0x3)
    elif layout == "older_tail":            # older large pool with type1/4 in its tail, no small pool
        pool = w(0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x00524C00, addr["t5"][1], addr["t5"][0],
                 0x0053FE18, addr["db"][1], addr["db"][0], 0x00527000,
                 addr["code"][1], addr["code"][0], addr["data"][1], addr["data"][0], 0x7, 0x8, 0x9,
                 addr["t1"][1], addr["t1"][0], addr["t4"][1], addr["t4"][0], 0x3ED)
    else:
        small = w(0x00524C00, addr["t5"][1], addr["t5"][0], addr["t1"][1], addr["t1"][0],
                  addr["t4"][1], addr["t4"][0], 0x3ED)
        pool = small + w(0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x00524C00, addr["t5"][1], addr["t5"][0],
                         0x0053FE18, addr["db"][1], addr["db"][0], 0x00527000,
                         addr["code"][1], addr["code"][0], addr["data"][1], addr["data"][0])
    c += pool + b"\0" * 64
    return bytes(c), addr


class NwmfwTest(unittest.TestCase):
    def check(self, code, addr, main="type4", key="t4"):
        parts, info = nwmfw.extract(code, main)
        plain = code if info["input"] == "raw" else nwmfw.blz_decompress(code)
        sl = lambda k: plain[addr[k][0] - BASE:addr[k][1] - BASE]  # noqa: E731
        self.assertEqual(parts["main"], sl(key))
        self.assertEqual(parts["database"], sl("db"))
        self.assertEqual(parts["stub_code"], sl("code"))
        self.assertEqual(parts["stub_data"], sl("data"))
        blob = nwmfw.pack(parts)
        magic, ver, n, chip = struct.unpack_from("<IIII", blob)
        self.assertEqual((magic, ver, n, chip), (0x46573349, 1, 4, 0x0D000001))
        want = {1: (0x524C00, "data"), 2: (0x527000, "code"), 3: (0x53FE18, "db"), 4: (0x524C00, key)}
        for i in range(n):
            kind, dst, off, ln = struct.unpack_from("<IIII", blob, 16 + 16 * i)
            self.assertEqual(dst, want[kind][0])
            self.assertEqual(blob[off:off + ln], sl(want[kind][1]))
            self.assertEqual(off % 4, 0)
        self.assertLess(len(blob), 64 * 1024)          # ar6k.c fw_buf
        return info

    def c_extract(self, code, main=4):
        """boot/arm9/wifi/nwmcode.c (the ARM9 loader) on the same input"""
        if not os.path.exists(NWM_HOST):
            self.skipTest("build/wifi-tests/nwm_host not built (make -C tools/ios3ds-wifi test)")
        with tempfile.TemporaryDirectory() as td:
            src, dst = os.path.join(td, "nwm.code"), os.path.join(td, "out.bin")
            with open(src, "wb") as f:
                f.write(code)
            r = subprocess.run([NWM_HOST, src, dst, str(main)], capture_output=True, text=True)
            if r.returncode:
                return None, r.stdout
            with open(dst, "rb") as f:
                return f.read(), r.stdout

    def test_c_loader_matches_python(self):
        class Few(random.Random):
            def getrandbits(self, k):
                return (0x00, 0x01, 0xE5, 0x9F)[int(self.random() * 4)]
        cases = [(make_code("newer")[0], 4), (make_code("newer")[0], 1), (make_code("older")[0], 4),
                 (make_code("older_tail")[0], 4), (make_code("older_tail")[0], 1),
                 (make_code("older")[0], 5), (blz_compress(make_code("newer", Few(3))[0], 0x200), 4)]
        for code, main in cases:
            parts, _ = nwmfw.extract(code, "type%d" % main)
            got, out = self.c_extract(code, main)
            self.assertIsNotNone(got, out)
            self.assertEqual(got, nwmfw.pack(parts), out)
        for bad in (bytes(4096), make_code("newer", bad_stub=True)[0]):
            got, out = self.c_extract(bad)
            self.assertIsNone(got)
            with self.assertRaises(nwmfw.FwError):
                nwmfw.extract(bad)

    def test_older_tail_pool(self):
        code, addr = make_code("older_tail")
        self.assertEqual(self.check(code, addr)["layout"], "older")
        self.check(code, addr, "type1", "t1")

    def test_newer_pool(self):
        code, addr = make_code("newer")
        info = self.check(code, addr)
        self.assertEqual((info["layout"], info["text_base"]), ("newer", BASE))
        self.check(code, addr, "type1", "t1")

    def test_older_pool(self):
        code, addr = make_code("older")
        self.assertEqual(self.check(code, addr)["layout"], "older")
        self.check(code, addr, "type5", "t5")

    def test_bad_sizes_rejected(self):
        code, _ = make_code("newer", bad_stub=True)
        with self.assertRaises(nwmfw.FwError):
            nwmfw.extract(code)

    def test_not_nwm(self):
        with self.assertRaises(nwmfw.FwError):
            nwmfw.extract(bytes(4096))

    def test_blz_roundtrip(self):
        rnd = random.Random(5)
        plain = bytes(rnd.choice(b"ABCD") for _ in range(3000)) + bytes(500) + b"xyz" * 300
        comp = blz_compress(plain, 100)
        self.assertLess(len(comp), len(plain))
        self.assertEqual(nwmfw.blz_decompress(comp), plain)

    def test_blz_code(self):
        class Few(random.Random):           # compressible "code"
            def getrandbits(self, k):
                return (0x00, 0x01, 0xE5, 0x9F)[int(self.random() * 4)]
        code, addr = make_code("newer", Few(2))
        comp = blz_compress(code, 0x200)
        self.assertLess(len(comp), len(code))
        self.assertEqual(self.check(comp, addr)["input"], "blz")


if __name__ == "__main__":
    unittest.main()
