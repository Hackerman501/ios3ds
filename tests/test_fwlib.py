"""Offline tests for scripts/fwlib + mkfirm. Synthetic data only, no Apple files."""
import base64
import hashlib
import io
import os
import plistlib
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))

from fwlib import appledt, lzss, udif, vfdecrypt  # noqa: E402
from fwlib.crypto import hmac_sha1  # noqa: E402
from fwlib.img3 import Img3  # noqa: E402

try:
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

    def enc(key, iv, data):
        e = Cipher(algorithms.AES(key), modes.CBC(iv)).encryptor()
        return e.update(data) + e.finalize()
except ImportError:  # pragma: no cover
    enc = None


class TestLzss(unittest.TestCase):
    def test_literals_and_backref(self):
        # 8 literals 'abcdabcd' then a back-reference copying 6 bytes from ring pos of 'a'
        r0 = 4096 - 18  # first literal lands here
        stream = bytes([0xFF]) + b"abcdabcd"
        pos = r0  # position of first 'a'
        length = 6
        stream += bytes([0x00, pos & 0xFF, ((pos >> 4) & 0xF0) | (length - 3)])
        out = lzss.lzss_decompress(stream, 14)
        self.assertEqual(out, b"abcdabcdabcdab")

    def test_complzss_header(self):
        payload = b"hello world!"
        comp = bytes([0xFF]) + payload[:8] + bytes([0x0F]) + payload[8:]
        hdr = struct.pack(">4s4sIII", b"comp", b"lzss", zlib.adler32(payload), len(payload), len(comp))
        blob = hdr + b"\0" * (0x180 - len(hdr)) + comp
        out, info = lzss.unpack_complzss(blob)
        self.assertEqual(out, payload)
        self.assertTrue(info["adler32_ok"])


@unittest.skipIf(enc is None, "needs python cryptography")
class TestImg3(unittest.TestCase):
    def build(self, plain, key, iv):
        ct = enc(key, iv, plain + b"\0" * (-len(plain) % 16))
        def tag(name, data, total=None):
            total = total or 12 + len(data)
            return struct.pack("<3I", struct.unpack(">I", name)[0], total, len(data)) + data
        data_tag = struct.pack("<3I", struct.unpack(">I", b"DATA")[0], 12 + len(ct), len(plain)) + ct
        body = tag(b"TYPE", b"lnrk") + data_tag
        return struct.pack("<5I", 0x496D6733, 20 + len(body), len(body), 0, struct.unpack(">I", b"krnl")[0]) + body

    def test_decrypt_unaligned_length(self):
        key, iv = os.urandom(16), os.urandom(16)
        plain = os.urandom(1000)  # not a multiple of 16: tail lives in tag padding
        im = Img3(self.build(plain, key, iv))
        self.assertEqual(im.ident, "krnl")
        self.assertEqual(im.data(key.hex(), iv.hex()), plain)


class TestDeviceTree(unittest.TestCase):
    def test_roundtrip_and_edit(self):
        root = appledt.Node()
        root.set("name", b"device-tree\0")
        chosen = appledt.Node()
        chosen.set("name", b"chosen\0")
        chosen.set("memory-map", b"\0" * 8, placeholder=True)
        root.children.append(chosen)
        blob = appledt.serialise(root)
        again = appledt.parse(blob)
        self.assertEqual(appledt.serialise(again), blob)
        self.assertIsNotNone(again.find("/chosen"))
        again.find("chosen").set("boot-args", b"-v\0")
        self.assertEqual(appledt.parse(appledt.serialise(again)).find("chosen").get("boot-args"), b"-v\0")


@unittest.skipIf(enc is None, "needs python cryptography")
class TestVfdecrypt(unittest.TestCase):
    def test_stream(self):
        key = os.urandom(36)
        plain = os.urandom(4096 * 3 + 100)
        hdr = bytearray(0x48)
        hdr[:8] = b"encrcdsa"
        struct.pack_into(">I", hdr, 8, 2)
        struct.pack_into(">I", hdr, 0x34, 4096)
        struct.pack_into(">QQ", hdr, 0x38, len(plain), 0x200)
        img = bytes(hdr) + b"\0" * (0x200 - 0x48)
        padded = plain + b"\0" * (-len(plain) % 4096)
        for i in range(len(padded) // 4096):
            iv = hmac_sha1(key[16:], struct.pack(">I", i))[:16]
            img += enc(key[:16], iv, padded[i * 4096:(i + 1) * 4096])
        out = io.BytesIO()
        vfdecrypt.decrypt_stream(io.BytesIO(img), out, key.hex())
        self.assertEqual(out.getvalue(), plain)


class TestUdif(unittest.TestCase):
    def test_zlib_raw_zero(self):
        s0, s1 = os.urandom(512 * 2), os.urandom(512)
        data = zlib.compress(s0) + s1
        c0 = (udif.T_ZLIB, 0, 0, 2, 0, len(zlib.compress(s0)))
        c1 = (udif.T_RAW, 0, 2, 1, len(zlib.compress(s0)), 512)
        c2 = (udif.T_ZERO, 0, 3, 2, 0, 0)
        c3 = (udif.T_END, 0, 5, 0, 0, 0)
        mish = struct.pack(">4sIQQQII", b"mish", 1, 0, 5, 0, 0, 0) + b"\0" * (200 - 40)
        mish += struct.pack(">I", 4) + b"".join(struct.pack(">IIQQQQ", *c) for c in (c0, c1, c2, c3))
        xml = plistlib.dumps({"resource-fork": {"blkx": [{"Name": "disk image (Apple_HFS : 1)", "Data": mish}]}})
        koly = bytearray(512)
        koly[:4] = b"koly"
        struct.pack_into(">QQ", koly, 24, 0, len(data))
        struct.pack_into(">QQ", koly, 216, len(data), len(xml))
        struct.pack_into(">Q", koly, 492, 5)
        f = io.BytesIO(data + xml + bytes(koly))
        parts = udif.partitions(f)
        self.assertEqual(len(parts), 1)
        out = io.BytesIO()
        udif.extract(f, out, parts[0])
        self.assertEqual(out.getvalue(), s0 + s1 + b"\0" * 1024)


class TestMkfirm(unittest.TestCase):
    def test_build(self):
        with tempfile.TemporaryDirectory() as d:
            a9, a11 = os.path.join(d, "a9.bin"), os.path.join(d, "a11.bin")
            with open(a9, "wb") as fh:
                fh.write(b"\x01" * 100)
            with open(a11, "wb") as fh:
                fh.write(b"\x02" * 700)
            out = os.path.join(d, "x.firm")
            subprocess.run([sys.executable, os.path.join(ROOT, "scripts", "mkfirm.py"), out,
                            "--arm9-entry", "0x08000040", "--arm11-entry", "0x1FF80000",
                            "--section", "0x08000040:" + a9, "--section", "0x1FF80000:" + a11,
                            "--screen-init"], check=True, capture_output=True)
            with open(out, "rb") as fh:
                f = fh.read()
            magic, prio, e11, e9 = struct.unpack_from("<4sIII", f)
            self.assertEqual((magic, e11, e9, f[0x10]), (b"FIRM", 0x1FF80000, 0x08000040, 1))
            off, addr, size, _m, h = struct.unpack_from("<IIII32s", f, 0x70)
            self.assertEqual((addr, size), (0x1FF80000, 1024))
            self.assertEqual(hashlib.sha256(f[off:off + size]).digest(), h)

    def test_rejects_bad_range(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "a.bin")
            with open(p, "wb") as fh:
                fh.write(b"x")
            r = subprocess.run([sys.executable, os.path.join(ROOT, "scripts", "mkfirm.py"),
                                os.path.join(d, "x.firm"), "--arm9-entry", "0x30000000",
                                "--section", "0x30000000:" + p], capture_output=True)
            self.assertNotEqual(r.returncode, 0)


if __name__ == "__main__":
    unittest.main()


sys.path.insert(0, os.path.join(ROOT, "kernel"))
import patcher  # noqa: E402
import dtgen  # noqa: E402


class TestPatcher(unittest.TestCase):
    def _write(self, d, recipes):
        import json
        p = os.path.join(d, "p.json")
        with open(p, "w") as f:
            json.dump(recipes, f)
        return p

    def test_apply_and_symbol(self):
        img = bytearray(b"\x00" * 16 + bytes.fromhex("0c309fe57000efe6") + b"\x00" * 8)
        with tempfile.TemporaryDirectory() as d:
            p = self._write(d, [{"name": "t", "va": "0x80001010", "orig": "0c309fe57000efe6",
                                 "new": "04f01fe5@shim+0x4@"}])
            done = patcher.apply_all(img, 0x80001000, p, {"shim": 0x80826000})
        self.assertEqual(len(done), 1)
        self.assertEqual(img[16:24], bytes.fromhex("04f01fe5") + struct.pack("<I", 0x80826004))

    def test_refuses_mismatch(self):
        img = bytearray(32)
        with tempfile.TemporaryDirectory() as d:
            p = self._write(d, [{"name": "t", "va": "0x80001000", "orig": "11223344", "new": "00000000"}])
            with self.assertRaises(ValueError):
                patcher.apply_all(img, 0x80001000, p)

    def test_repo_patches_are_well_formed(self):
        import json
        with open(os.path.join(ROOT, "kernel", "patches.json")) as f:
            for p in json.load(f):
                new = patcher._expand(p["new"], {"shim": 0})
                self.assertEqual(len(bytes.fromhex(p["orig"])), len(bytes.fromhex(new)), p["name"])


class TestDtgen(unittest.TestCase):
    def _node(self, name, props=(), children=()):
        n = appledt.Node()
        n.set("name", name.encode() + b"\0")
        for k, v in props:
            n.set(k, v)
        n.children.extend(children)
        return n

    def _template(self):
        N = self._node
        mm = N("memory-map", [("MemoryMapReserved-%d" % i, b"\0" * 8) for i in range(4)])
        chosen = N("chosen", [("debug-enabled", b"\0" * 4), ("firmware-version", b"\0" * 32)], [mm])
        cpu = N("cpu0", [(k, b"\0" * 4) for k in ("clock-frequency", "bus-frequency",
                "peripheral-frequency", "memory-frequency", "fixed-frequency", "timebase-frequency")])
        armio = N("arm-io", [("ranges", b"\0" * 24)], [
            N("timer", [("reg", struct.pack("<II", 0x4700000, 0x1000))]),
            N("clcd", [("reg", struct.pack("<II", 0x900000, 0x2000)), ("function-lcd_enable", b"\0" * 8)]),
            N("dmac0", [("reg", struct.pack("<II", 0x200000, 0x1000))])])
        root = N("device-tree", [("serial-number", b"\0" * 32), ("secure-root-prefix", b"md\0\0")], [
            chosen, N("cpus", [], [cpu]), N("memory", [("reg", b"\0" * 8)]),
            N("pram", [("reg", b"\0" * 8)]), N("vram", [("reg", b"\0" * 8)]), armio])
        return appledt.serialise(root)

    def test_generate(self):
        saved = (dtgen.FAKEIO, dtgen.DELETE)
        dtgen.FAKEIO, dtgen.DELETE = ["timer", "clcd"], ["arm-io/dmac0"]
        try:
            lay = {"devtree_pa": 0x2081b000, "bootargs_pa": 0x20825000, "ramdisk_len": 0x40000000}
            r = appledt.parse(dtgen.generate(self._template(), lay, 0x20000000, 0x80000000, 0x08000000, 0))
        finally:
            dtgen.FAKEIO, dtgen.DELETE = saved
        self.assertEqual(r.find("chosen").get("debug-enabled"), struct.pack("<I", 1))
        self.assertEqual(r.find("memory").get("reg"), struct.pack("<II", 0x20000000, 0x08000000))
        mm = r.find("chosen/memory-map")
        self.assertEqual(mm.get("DeviceTree"), struct.pack("<II", 0x2081b000, 0xa000))
        self.assertEqual(mm.get("RAMDisk"), struct.pack("<II", 0x20000000, 0x40000000))
        self.assertEqual(r.find("pram").get("reg"), struct.pack("<II", *dtgen.PRAM))
        self.assertIsNone(r.get("secure-root-prefix"))
        self.assertIsNone(r.find("arm-io/dmac0"))
        self.assertIsNone(r.find("arm-io/clcd").get("function-lcd_enable"))
        # fake pages: timer -> page 0, clcd -> pages 1-2 (child addresses relative to ranges[0])
        self.assertEqual(r.find("arm-io/timer").get("reg"), struct.pack("<II", 0x0000, 0x1000))
        self.assertEqual(r.find("arm-io/clcd").get("reg"), struct.pack("<II", 0x1000, 0x2000))
        self.assertEqual(r.find("arm-io").get("ranges")[:12], struct.pack("<III", 0, dtgen.FAKEIO_BASE, dtgen.FAKEIO_PAGES * 0x1000))
        img = dtgen.fakeio_image()
        self.assertEqual(struct.unpack_from("<I", img, 0x1000 + 0x24)[0], 0x18000000)   # CLCD window base
