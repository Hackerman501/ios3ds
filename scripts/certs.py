#!/usr/bin/env python3
"""Current root certificates for iOS 4.2.1's trust store (docs/certificates.md).

iOS 4.2.1's securityd builds every SecTrust chain (Safari, NSURLConnection/Cydia,
Mail, ...) from two anchor sources: the roots compiled into securityd (2010) and the
user trust store /private/var/Keychains/TrustStore.sqlite3 (table tsettings; a
certificate is an anchor if a row with its SHA-1 exists, and it is found as a parent
by its normalized subject). This script writes that database at build time from
rootfs/certs/roots.pem, a list taken from Mozilla's CA bundle.

  scripts/certs.py db OUT.sqlite3 [--pem rootfs/certs/roots.pem]
  scripts/certs.py update [--from BUNDLE] [--extra-from OLD --extra CN ...]   # refresh roots.pem
"""
import argparse
import base64
import datetime
import hashlib
import os
import re
import sqlite3
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PEM = os.path.join(ROOT, "rootfs", "certs", "roots.pem")
PEM_EC = os.path.join(ROOT, "rootfs", "certs", "roots-ec.pem")
# securityd's own schema (strings in /usr/libexec/securityd, 8C148)
SCHEMA = ("CREATE TABLE tsettings(sha1 BLOB NOT NULL DEFAULT '',subj BLOB NOT NULL DEFAULT '',"
          "tset BLOB,data BLOB,PRIMARY KEY(sha1));CREATE INDEX isubj ON tsettings(subj);")
HOST_BUNDLES = ["/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt",
                "/etc/ssl/cert.pem", "/usr/local/etc/openssl/cert.pem"]

OID_RSA = bytes.fromhex("2a864886f70d010101")
OID_EC = bytes.fromhex("2a8648ce3d0201")          # id-ecPublicKey
OID_EC = bytes.fromhex("2a8648ce3d0201")
PRINTABLE = 0x13


# ---- minimal DER ---------------------------------------------------------------
def tlv(b, i):
    """-> (tag, content_start, end) of the element at b[i]."""
    tag, n = b[i], b[i + 1]
    i += 2
    if n & 0x80:
        k = n & 0x7f
        n = int.from_bytes(b[i:i + k], "big")
        i += k
    return tag, i, i + n


def children(b, start, end):
    out, i = [], start
    while i < end:
        t, s, e = tlv(b, i)
        out.append((t, s, e, i))
        i = e
    return out


def enc(tag, content):
    n = len(content)
    if n < 0x80:
        ln = bytes([n])
    else:
        nb = n.to_bytes((n.bit_length() + 7) // 8, "big")
        ln = bytes([0x80 | len(nb)]) + nb
    return bytes([tag]) + ln + content


def normalize_name(b, start, end):
    """Content of an X.501 Name normalized like iOS 4's SecCertificate does it
    (verified against the 2010 anchors compiled into securityd): PrintableString
    values upper-cased (ASCII), leading/trailing spaces removed and runs of spaces
    collapsed; every other string type kept as is; lengths re-encoded."""
    out = b""
    for rt, rs, re_, _ in children(b, start, end):          # RDN SETs
        rdn = b""
        for at, as_, ae, _ in children(b, rs, re_):         # AttributeTypeAndValue
            (ot, os_, oe, oi), (vt, vs, ve, vi) = children(b, as_, ae)[:2]
            val = b[vs:ve]
            if vt == PRINTABLE:
                val = re.sub(rb" +", b" ", val.upper()).strip(b" ")
            rdn += enc(at, b[oi:oe] + enc(vt, val))
        out += enc(rt, rdn)
    return out


class Cert:
    def __init__(self, der):
        self.der = der
        _, cs, ce = tlv(der, 0)
        tbs = children(der, cs, ce)[0]
        f = children(der, tbs[1], tbs[2])
        if f[0][0] == 0xa0:                                  # version
            f = f[1:]
        self.sig_alg = der[f[1][1] + 2:children(der, f[1][1], f[1][2])[0][2]]
        self.issuer = f[2]
        val = children(der, f[3][1], f[3][2])
        self.not_after = self._time(der, val[1])
        self.subject = f[4]
        spki = children(der, f[5][1], f[5][2])
        alg = children(der, spki[0][1], spki[0][2])[0]
        self.key_oid = der[alg[1]:alg[2]]
        bits = der[spki[1][1] + 1:spki[1][2]]
        if self.key_oid == OID_RSA:
            mod = children(bits, *tlv(bits, 0)[1:])[0]
            m = bits[mod[1]:mod[2]].lstrip(b"\0")
            self.key_bits = len(m) * 8
        else:
            self.key_bits = 0

    @staticmethod
    def _time(b, el):
        t, s, e, _ = el
        s = b[s:e].decode()
        fmt = "%y%m%d%H%M%SZ" if t == 0x17 else "%Y%m%d%H%M%SZ"
        return datetime.datetime.strptime(s, fmt)

    def subj(self):
        return normalize_name(self.der, self.subject[1], self.subject[2])

    def sha1(self):
        return hashlib.sha1(self.der).digest()

    def cn(self):
        """Subject CN (else O/OU), for comments and logs."""
        names = {}
        for _, rs, re_, _ in children(self.der, self.subject[1], self.subject[2]):
            for _, as_, ae, _ in children(self.der, rs, re_):
                (_, os_, oe, _), (vt, vs, ve, _) = children(self.der, as_, ae)[:2]
                v = self.der[vs:ve]
                v = v.decode("utf-16-be") if vt == 0x1e else v.decode("utf-8", "replace")
                names.setdefault(self.der[os_:oe].hex(), v)
        return names.get("550403") or names.get("55040a") or names.get("55040b") or "?"


def read_pem(path):
    with open(path) as f:
        txt = f.read()
    out = []
    for m in re.finditer(r"-----BEGIN CERTIFICATE-----(.*?)-----END CERTIFICATE-----", txt, re.S):
        out.append(base64.b64decode("".join(m.group(1).split())))
    return out


def usable(c, now=None, ec=False):
    """iOS 4.2.1's Security.framework verifies RSA signatures only (md5/sha1/sha256/
    sha384/sha512 with RSA; its only ECDSA OID is ecdsa-with-SHA1 and it has no P-256/
    P-384 curve), so EC roots can never anchor a chain there; ec=True selects exactly
    those, for TLSFix's own OpenSSL check of EC chains (write_profile). Expired roots
    are useless."""
    now = now or datetime.datetime.now(datetime.timezone.utc).replace(tzinfo=None)
    if ec and c.key_oid != OID_EC:
        return False, "not EC"
    if not ec and c.key_oid != OID_RSA:
        return False, "not RSA"
    if c.not_after < now:
        return False, "expired"
    return True, ""


def write_db(out, pems):
    if os.path.exists(out):
        os.remove(out)
    db = sqlite3.connect(out)
    db.executescript(SCHEMA)
    n = 0
    for der in pems:
        c = Cert(der)
        db.execute("INSERT OR REPLACE INTO tsettings(sha1,subj,tset,data) VALUES(?,?,?,?)",
                   (c.sha1(), c.subj(), None, der))
        n += 1
    db.commit()
    db.close()
    return n


def write_profile(out, pems, ident="org.ios3ds.roots"):
    """The roots as a configuration-profile plist (one com.apple.security.root payload
    each). TLSFix 1.3 verifies chains with EC certificates, which iOS 4.2.1 cannot,
    with OpenSSL against the CA payloads of the files in
    /var/mobile/Library/ConfigurationProfiles (its tlsfix_ecc.c); this file is that
    anchor list. iOS itself lists only the profiles it installed (*.stub)."""
    import plistlib
    payloads = []
    for der in pems:
        c = Cert(der)
        payloads.append({"PayloadType": "com.apple.security.root", "PayloadVersion": 1,
                         "PayloadIdentifier": "%s.%s" % (ident, c.sha1().hex()),
                         "PayloadUUID": c.sha1().hex(), "PayloadDisplayName": c.cn(),
                         "PayloadContent": der})
    with open(out, "wb") as f:
        plistlib.dump({"PayloadType": "Configuration", "PayloadVersion": 1, "PayloadIdentifier": ident,
                       "PayloadUUID": ident, "PayloadDisplayName": "ios3ds root certificates",
                       "PayloadContent": payloads}, f, fmt=plistlib.FMT_XML)
    return len(payloads)


def update(src, out, extra_src=None, extra=(), ec=False):
    """Regenerate roots.pem from a Mozilla-derived bundle (host ca-certificates/certifi),
    plus the roots named in `extra` taken from another (older) bundle; ec=True: the
    EC roots instead (roots-ec.pem)."""
    keep, skipped, seen = [], [], set()
    pool = [(der, True) for der in read_pem(src)]
    if extra_src:
        pool += [(der, False) for der in read_pem(extra_src)]
    for der, primary in pool:
        c = Cert(der)
        if c.sha1() in seen or (not primary and c.cn() not in extra):
            continue
        ok, why = usable(c, ec=ec)
        if ok:
            seen.add(c.sha1())
            keep.append(c)
        else:
            skipped.append((c, why))
    missing = set(extra) - {c.cn() for c in keep}
    if missing:
        sys.exit("[certs] extra roots not found/usable: " + ", ".join(sorted(missing)))
    keep.sort(key=lambda c: c.cn().lower())
    with open(out, "w") as f:
        if ec:
            f.write("# EC root certificates for TLSFix's check of EC chains (scripts/certs.py update --ec,\n"
                    "# docs/certificates.md): the EC roots of Mozilla's CA bundle (%s),\n"
                    "# generated %s. iOS 4.2.1's own trust store cannot use them.\n\n"
                    % (os.path.basename(src), datetime.date.today().isoformat()))
        else:
            f.write("# Root certificates added to iOS 4.2.1's trust store (scripts/certs.py update,\n"
                    "# docs/certificates.md): the RSA roots of Mozilla's CA bundle (%s)%s,\n"
                    "# generated %s. EC and expired roots are left out (iOS 4.2.1 cannot use them).\n\n"
                    % (os.path.basename(src), " plus " + ", ".join(sorted(extra)) if extra else "",
                       datetime.date.today().isoformat()))
        for c in keep:
            b64 = base64.b64encode(c.der).decode()
            f.write("# %s (%s, until %s)\n-----BEGIN CERTIFICATE-----\n%s\n-----END CERTIFICATE-----\n\n"
                    % (c.cn(), "EC" if ec else "RSA %d" % c.key_bits, c.not_after.date(),
                       "\n".join(b64[i:i + 64] for i in range(0, len(b64), 64))))
    print("[certs] %s: %d roots, %d skipped (%s)" % (out, len(keep), len(skipped),
          ", ".join(sorted(set(w for _, w in skipped)))))


def main():
    ap = argparse.ArgumentParser()
    sp = ap.add_subparsers(dest="cmd", required=True)
    d = sp.add_parser("db")
    d.add_argument("out")
    d.add_argument("--pem", default=PEM)
    u = sp.add_parser("update")
    u.add_argument("--from", dest="src")
    u.add_argument("--out", default=PEM)
    u.add_argument("--extra-from", help="older bundle to take --extra roots from")
    u.add_argument("--extra", action="append", default=[], help="CN of a root to add from --extra-from")
    u.add_argument("--ec", action="store_true", help="the EC roots, into roots-ec.pem")
    a = ap.parse_args()
    if a.cmd == "db":
        print("[certs] %s: %d roots" % (a.out, write_db(a.out, read_pem(a.pem))))
    else:
        src = a.src
        if not src:
            src = next((p for p in HOST_BUNDLES if os.path.exists(p)), None)
        if not src:
            try:
                import certifi
                src = certifi.where()
            except ImportError:
                sys.exit("no CA bundle found; pass --from")
        out = a.out if not a.ec or a.out != PEM else PEM_EC
        update(src, out, a.extra_from, a.extra, ec=a.ec)


if __name__ == "__main__":
    main()
