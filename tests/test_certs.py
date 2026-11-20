"""Offline tests of the root-certificate update (scripts/certs.py,
docs/certificates.md): subject normalization as iOS 4.2.1 does it, the
TrustStore.sqlite3 securityd reads, and the committed roots.pem."""
import datetime
import os
import sqlite3
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import certs  # noqa: E402


def atv(oid_hex, tag, value):
    return certs.enc(0x30, certs.enc(0x06, bytes.fromhex(oid_hex)) + certs.enc(tag, value))


def name(*atvs):
    """Name SEQUENCE with one RDN SET per ATV"""
    return certs.enc(0x30, b"".join(certs.enc(0x31, a) for a in atvs))


class Normalize(unittest.TestCase):
    def norm(self, der):
        _, s, e = certs.tlv(der, 0)
        return certs.normalize_name(der, s, e)

    def test_printable_upper_and_spaces(self):
        der = name(atv("550406", 0x13, b"us"), atv("550403", 0x13, b"  Foo   Bar  CA "))
        want = (certs.enc(0x31, atv("550406", 0x13, b"US")) +
                certs.enc(0x31, atv("550403", 0x13, b"FOO BAR CA")))
        self.assertEqual(self.norm(der), want)

    def test_other_string_types_kept(self):
        # UTF8String / IA5String stay as they are (AffirmTrust, Certigna, ... in securityd's table)
        der = name(atv("55040a", 0x0c, b"AffirmTrust  x"), atv("2a864886f70d010901", 0x16, b"a@B.c"))
        _, s, e = certs.tlv(der, 0)
        self.assertEqual(self.norm(der), der[s:e])

    def test_long_lengths_reencoded(self):
        long = b"a" * 150 + b"   "
        der = name(atv("550403", 0x13, long))
        self.assertEqual(self.norm(der), certs.enc(0x31, atv("550403", 0x13, b"A" * 150)))


class Roots(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.ders = certs.read_pem(certs.PEM)
        cls.certs = [certs.Cert(d) for d in cls.ders]

    def test_required_roots(self):
        names = {c.cn() for c in self.certs}
        for n in ("ISRG Root X1", "DigiCert Global Root G2", "GTS Root R1", "GTS Root R2",
                  "USERTrust RSA Certification Authority", "Amazon Root CA 1",
                  "Sectigo Public Server Authentication Root R46", "GlobalSign"):
            self.assertIn(n, names)

    def test_all_usable(self):
        # RSA only (iOS 4.2.1 has no P-256/P-384), none expired within a year of generation
        soon = datetime.datetime(2027, 6, 1)
        for c in self.certs:
            self.assertEqual(c.key_oid, certs.OID_RSA, c.cn())
            self.assertGreater(c.not_after, soon, c.cn())
        self.assertEqual(len({c.sha1() for c in self.certs}), len(self.certs))

    def test_db(self):
        with tempfile.TemporaryDirectory() as td:
            p = os.path.join(td, "TrustStore.sqlite3")
            n = certs.write_db(p, self.ders)
            self.assertEqual(n, len(self.ders))
            db = sqlite3.connect(p)
            sql = db.execute("SELECT sql FROM sqlite_master WHERE name='tsettings'").fetchone()[0]
            self.assertIn("sha1 BLOB NOT NULL DEFAULT ''", sql)
            for c in self.certs[:5]:
                # securityd's own queries (SecTrustStoreContains / CopyParents)
                self.assertIsNotNone(db.execute("SELECT tset FROM tsettings WHERE sha1=?",
                                                (c.sha1(),)).fetchone())
                rows = db.execute("SELECT data FROM tsettings WHERE subj=?", (c.subj(),)).fetchall()
                self.assertIn((c.der,), rows)
            db.close()
            with open(p, "rb") as f:
                hdr = f.read(100)
            self.assertEqual(hdr[18:20], b"\x01\x01")      # rollback journal, readable by SQLite 3.6
            self.assertEqual(hdr[44:48], b"\x00\x00\x00\x04")

    def test_ec_roots_and_profile(self):
        """roots-ec.pem + the profile TLSFix 1.3 reads its EC-chain anchors from."""
        import plistlib
        ec = [certs.Cert(d) for d in certs.read_pem(certs.PEM_EC)]
        names = {c.cn() for c in ec}
        for n in ("ISRG Root X2", "GTS Root R4", "DigiCert Global Root G3", "USERTrust ECC Certification Authority"):
            self.assertIn(n, names)
        soon = datetime.datetime(2027, 6, 1)
        for c in ec:
            self.assertEqual(c.key_oid, certs.OID_EC, c.cn())
            self.assertGreater(c.not_after, soon, c.cn())
        with tempfile.TemporaryDirectory() as td:
            p = os.path.join(td, "roots.plist")
            ders = self.ders + [c.der for c in ec]
            self.assertEqual(certs.write_profile(p, ders), len(ders))
            with open(p, "rb") as f:
                pl = plistlib.load(f)
            pay = pl["PayloadContent"]
            self.assertEqual({x["PayloadType"] for x in pay}, {"com.apple.security.root"})
            self.assertEqual([x["PayloadContent"] for x in pay], ders)


if __name__ == "__main__":
    unittest.main()
