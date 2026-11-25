# HTTPS: current root certificates for iOS 4.2.1

iOS 4.2.1's list of trusted root certificates is from 2010. Most HTTPS sites today
chain to roots created after that (ISRG Root X1 for Let's Encrypt, DigiCert Global
Root G2, ...), so Safari and Cydia said "The certificate for this server is
invalid" for them, Cydia's home page and package depictions on
https://cydia.saurik.com included.

ios3ds images now ship a **current root store**: the RSA roots of Mozilla's CA
bundle, added at build time as anchors that iOS's own certificate check uses.
Certificate checking stays on: an untrusted, expired or wrong-name certificate is
still refused.

What it can't fix is the **protocol**: iOS 4.2.1 speaks TLS 1.0 at most (section 4).

## 1. How iOS 4.2.1 decides what is trusted (RE of securityd, 8C148)

Every certificate check on iOS 4.2.1 (Safari/WebKit, `NSURLConnection` as used by
Cydia, Mail, CFNetwork streams) ends in `SecTrustEvaluate`, and the chain is built
and checked inside `securityd` (`/usr/libexec/securityd`): its path builder is
the code that knows the request keys `certificates`, `anchors`, `anchorsOnly`.
Findings, all from the 8C148 `securityd` (ARM code, no symbols):

- **System anchors** are compiled into `securityd` itself: a table of 174
  certificates (`__DATA,__const` at 0x496e4: length, pointer, flags) plus a gperf-style
  hash table from *normalized subject* to index (0x49f1c, lookup code at 0x7f80..0x8360).
  101 of the 174 have expired by 2026; there is no file to replace.
- **User anchors** come from `/Library/Keychains/TrustStore.sqlite3` (the path is a
  literal; `/Library/Keychains` is a symlink to `/private/var/Keychains`). The store
  is opened at 0xa204 (`sqlite3_open`, creating the table
  `tsettings(sha1, subj, tset, data)` if missing) and two statements are prepared:
  `SELECT data FROM tsettings WHERE subj=?` (parents) and
  `SELECT tset FROM tsettings WHERE sha1=?` (contains).
- **Contains** (0x9a44) binds the certificate's SHA-1 and returns true when
  `sqlite3_step` gives a row: `tset` (the trust settings) is never looked at.
- **Copy parents** (0x9aec) binds `SecCertificateGetNormalizedIssuerContent(cert)`
  and returns every `data` row as a certificate.
- The path builder (0x8e40, `SecPathBuilderInit`) appends four certificate sources
  to its parent sources: the certificates the server sent, the **system anchors**
  (0x8364/0x8430), the **user anchors** (0x67c8/0x67a0, which call the trust store
  for domain 2 = user) and the network/CA-issuer source. Unless the caller set
  `anchorsOnly` (Safari and Cydia don't), the system and user anchor sources are
  also the anchor sources. So a row in `tsettings` makes that certificate a trusted
  root for **every app**, exactly like a certificate installed from a configuration
  profile (that is how profiles add roots on iOS 4).

So the build writes `TrustStore.sqlite3` with one row per root:
`sha1` = SHA-1 of the DER certificate, `subj` = its normalized subject, `tset` =
NULL, `data` = the DER certificate.

**Normalized subject:** the content of the subject Name (the RDN SETs, without the
outer SEQUENCE header), where every PrintableString value is upper-cased (ASCII),
stripped of leading/trailing spaces and has runs of spaces collapsed; other string
types (UTF8String, IA5String, ...) are kept as they are; lengths are re-encoded.
`scripts/certs.py` implements this and reproduces the normalized subject of **all
174** anchors compiled into `securityd` byte for byte (146 of them differ from the
raw subject), which is how it was verified. A wrong `subj` would make the root
unfindable as a parent (lookups go by the child's normalized issuer).

## 2. What is added

`rootfs/certs/roots.pem` (committed; these are public CA certificates, not Apple
files): **81 roots**, every RSA root in Mozilla's bundle (Ubuntu's
`ca-certificates` 20260816, which follows Mozilla's NSS list) that has not expired,
plus **GTS Root R2** (Google; dropped from that bundle, still valid until 2036)
from an older certifi. Among them:

| Root | Used by (examples) |
|---|---|
| ISRG Root X1 | Let's Encrypt (RSA chains: R10-R14, YR1-YR3 via Root YR) |
| DigiCert Global Root G2, DigiCert TLS RSA4096 Root G5, DigiCert Trusted Root G4 | cydia.saurik.com / apt.saurik.com (RapidSSL TLS RSA CA G1), many DigiCert sites |
| GTS Root R1, GTS Root R2 | Google (RSA certificates: WR2 -> GTS Root R1) |
| GlobalSign (R3, R6), GlobalSign Root R46 | GlobalSign customers |
| USERTrust RSA, Sectigo Public Server Authentication Root R46, COMODO RSA | Sectigo/Comodo (e.g. apt.thebigboss.org) |
| Amazon Root CA 1, Amazon Root CA 2 | AWS/CloudFront |
| Go Daddy / Starfield G2, Microsoft RSA Root 2017, IdenTrust, Certum, SSL.com, ... | |

**Left out, because iOS 4.2.1 can't use them:**

- **EC roots** (40 in the bundle: ISRG Root X2, DigiCert Global Root G3, GTS Root
  R3/R4, Amazon Root CA 3/4, GlobalSign R5/E46, USERTrust ECC, Sectigo E46, ...).
  4.2.1's Security.framework only knows RSA signature algorithms (md5/sha1/sha256/
  sha384/sha512 with RSA, all OIDs present) and, for EC, only the
  `ecdsa-with-SHA1` OID: there is no `ecdsa-with-SHA256/384` and no P-256/P-384
  curve OID in the library, so a chain through an EC root (or an EC intermediate)
  can never verify. EC *server* certificates don't matter here anyway: iOS 4's
  TLS offers no ECDSA cipher suites, and servers that have both send the RSA one.
- **Expired roots.**

Mozilla also dropped some old roots for age (GlobalSign Root CA R1, DigiCert Global
Root CA, AAA Certificate Services, ...); those are still in iOS 4.2.1's compiled-in
list and keep working where they haven't expired.

Refresh the list (e.g. once a year) on a Linux host:

    scripts/certs.py update                      # host's ca-certificates (or certifi)
    scripts/certs.py update --extra-from OLD.pem --extra "GTS Root R2"   # as committed

The build (`scripts/mkrootfs.py`, recipe key `trust_store` in `rootfs/recipe.json`)
writes `/private/var/Keychains/TrustStore.sqlite3` from `roots.pem` every time
(owner `_securityd` via the folder, SQLite file format 4 with a rollback journal,
which 4.2.1's SQLite 3.6.23 reads). Roots you install later from a profile go
into the same file. Remove the `trust_store` key to build the stock store.

## 3. Results (QEMU, ctr-wifi + slirp, `--icount 2`)

Same image with and without `trust_store` (the "before" column is the stock 2010
store). `emu/wifi_test.py --https ...` (CFNetwork in `ios3ds-nettest`) unless
noted; the UI rows were driven with `emu/session.py --wifi`.

| Site | Chain | Before | After |
|---|---|---|---|
| Cydia home page (Cydia app, cydia.saurik.com) | RapidSSL TLS RSA CA G1 -> DigiCert Global Root G2 | "certificate for this server is invalid" (re-log 60) | **loads** (Featured, Themes, Manage Account, help sections) |
| https://cydia.saurik.com/ | same | -9807 not trusted | **200** |
| https://tls-v1-0.badssl.com/ (Let's Encrypt) | YR1 -> Root YR -> ISRG Root X1 | -9807 not trusted | **200**; Safari shows the page with the lock icon |
| https://www.google.com/ (Safari) | WR2 -> GTS Root R1 (-> GlobalSign Root CA) | (chains to the stock GlobalSign Root CA) | **loads** with the lock icon. `ios3ds-nettest`'s bare CFNetwork request gets -9806 (server closes the handshake) from Google, Safari doesn't; not a certificate error |
| https://www.amazon.com/ | GeoTrust TLS RSA CA G1 -> DigiCert G2 -> VeriSign G5 (stock) | 400 (TLS ok) | unchanged |
| https://moreinfo.thebigboss.org/ (BigBoss) | Sectigo DV R36 -> Sectigo R46 -> USERTrust RSA, **all signed with SHA-384**, 3072-bit intermediate | -9807 | **still -9807** (see below) |
| https://letsencrypt.org/ | (EC chain) | -9836 | -9836: server requires TLS 1.2 (section 4) |
| https://untrusted-root.badssl.com/, https://expired.badssl.com/ | negative controls | -9807 | -9807: checking is still on |

**SHA-384 chains (corrected, re-log 71):** the BigBoss failure was not the
hash. Its certificate (Sectigo DV R36 -> R46 -> USERTrust RSA, all
`sha384WithRSAEncryption`) **expired on 2026-06-15**, and apt.thebigboss.org serves
the same certificate, issued for moreinfo.thebigboss.org. github.com's RSA
certificate is on the same SHA-384 hierarchy and verifies through
`SecTrustEvaluate` (with TLSFix, section 4), so 4.2.1 does check SHA-384 RSA
chains. BigBoss over HTTPS keeps failing until its owner renews the certificate;
the repository itself is plain HTTP and works.

Not needed: an opt-in "ignore certificate errors" switch. Renewing the roots fixed
Cydia's pages, so certificate checking stays on with no bypass.

## 4. TLS 1.2/1.3: TLSFix

iOS 4.2.1's SecureTransport speaks **SSL 3.0 and TLS 1.0 at most** (TLS 1.1/1.2
arrived with iOS 5), with CBC/RC4 cipher suites only. Many servers now accept
only TLS 1.2 or newer (letsencrypt.org, github.com, wikipedia.org, apple.com,
most of Cloudflare's defaults in 2026); with the stock stack they fail in the
handshake (`[nettest]`: -9836 `errSSLPeerProtocolVersion` / -9806).

ios3ds images therefore come with **TLSFix 1.3**
([ObscureMosquito/TLSFix](https://github.com/ObscureMosquito/TLSFix), a fork of
[nfzerox/TLSFix](https://github.com/nfzerox/TLSFix); MIT, notice in the README),
a Cydia Substrate tweak downloaded at build time like Cydia (`rootfs/cydia.json`,
pinned sha256, docs/cydia.md). It hooks the SecureTransport functions
(`SSLNewContext`, `SSLSetIOFuncs`, `SSLHandshake`, `SSLRead`, `SSLWrite`, ...)
and runs the connection through a statically linked **OpenSSL 1.1.1w** instead
(TLS 1.0 to 1.3, AES-GCM, ChaCha20, ECDHE, SNI), while CFNetwork keeps driving the
socket. It loads into every process that has CFNetwork and Cydia Substrate
(Cydia and other unsandboxed apps, `ios3ds-nettest`) except SpringBoard, launchd,
lockdownd and securityd. **Safari and App Store apps** run in the sandbox, which denies every file under
`/Library` but allows `/usr/lib`. The build therefore installs the two dylibs
TLSFix needs where they can be read: `/usr/lib/ios3ds-CydiaSubstrate.dylib`
(the CydiaSubstrate framework binary) and `/usr/lib/ios3ds-tlsfix.dylib`.
`ios3ds-applog`, which is already inserted into every app, falls back to them
when its `dlopen("/Library/MobileSubstrate/MobileSubstrate.dylib")` fails, and
loads TLSFix from a run-loop timer once CFNetwork is up (an app's constructors
run before its frameworks). Two strings inside the TLSFix copy are rewritten
because they point under `/Library` or `/var/mobile`: the `LC_LOAD_DYLIB` of
CydiaSubstrate and the ConfigurationProfiles directory, whose CA payload file
(`ios3ds-roots.plist`) is copied to `/usr/lib/ConfigurationProfiles/`. The old
opt-in kernel patch (`make sd SANDBOX=1`, docs/cydia.md) is not needed for this
and is no longer the way TLSFix reaches Safari.

**Certificate checks** stay iOS's: the server's chain is handed to
`SecTrustEvaluate` with an SSL policy for the host name, i.e. the trust store of
sections 1-2 (an expired, wrong-name or untrusted certificate is refused with
-9807 as before). 4.2.1 cannot verify EC keys or ECDSA signatures (section 2), so
for a chain that contains an EC certificate TLSFix 1.3 falls back to OpenSSL's
`X509_verify_cert` (purpose SSL server, host name, dates) against the CA payloads
of the files in `/var/mobile/Library/ConfigurationProfiles`. The build writes
`ios3ds-roots.plist` there (`trust_store.profile` in `rootfs/recipe.json`,
`certs.write_profile`): the 80 RSA roots of `roots.pem` plus the **40 EC roots** of
`rootfs/certs/roots-ec.pem` (Mozilla's EC roots: ISRG Root X2, GTS Root R3/R4,
DigiCert Global Root G3, USERTrust ECC, Sectigo E46, Amazon Root CA 3/4, ...;
`scripts/certs.py update --ec`). iOS lists only the profiles it installed
itself (`*.stub`), so the file does not show up in Settings.

Results (QEMU, ctr-wifi + slirp, `emu/wifi_test.py --https`, `ios3ds-nettest`
with Substrate + TLSFix 1.3; re-log 71):

| Site | Without TLSFix | With TLSFix 1.3 |
|---|---|---|
| https://github.com/ (ECDSA or RSA/SHA-384 Sectigo chain) | -9836 / -9806 | **200** (537 KB) |
| https://en.wikipedia.org/ (Let's Encrypt EC chain) | -9836 | **200** (267 KB) |
| https://www.apple.com/ (RSA, DigiCert G2) | -9836 | **200** (250 KB) |
| https://www.google.com/, https://cydia.saurik.com/ | 200 | 200 |
| https://ecc256.badssl.com/ (EC) | | 200 |
| https://letsencrypt.org/ | -9836 | TLS ok, then CFNetwork HTTP error -4 (domain 4) on the response |
| https://moreinfo.thebigboss.org/, https://apt.thebigboss.org/ | -9807 | -9807: certificate expired 2026-06-15 (section 3) |
| https://expired.badssl.com/, untrusted-root, wrong.host | -9807 | -9807 (still refused) |
| Cydia app home page | loads | loads (Substrate + TLSFix in Cydia) |
| Safari: github.com, www.apple.com | "could not establish a secure connection" | **loads**, lock icon (TLSFix via the `/usr/lib` copies, NEW) |
| Safari: local TLS 1.2-only server, RSA certificate | "could not establish a secure connection" | **loads**, server saw `TLSv1.2` (NEW) |
| Safari: local TLS 1.2-only server, P-256 ECDSA certificate | -9836 | **loads**, server saw `TLSv1.2`; OpenSSL verified the EC chain against `/usr/lib/ConfigurationProfiles` (NEW) |

**Cost** (`--icount 2`, about the Old 3DS ARM11 speed, whole request incl.
DNS and body): cydia.saurik.com 2 s with and without TLSFix, www.google.com 3 s
vs 2 s, www.apple.com 4 s and en.wikipedia.org 9 s (EC chain, OpenSSL check)
where the stock stack fails. TLSFix's armv6 code is 1.9 MB (file-backed, shared)
plus 144 KB `__DATA` per process that loads it; OpenSSL is set up on the first TLS
connection, not at launch.

**Safari in QEMU** (`--icount 2`, local TLS 1.2-only server on the host's slirp
address): without the copies Safari shows "Safari cannot open the page because it
could not establish a secure connection" and the host sees no TLS request; with
them the host logs `TLSv1.2` and Safari shows the page with the lock icon, for an
RSA certificate and for a P-256 ECDSA one (the latter goes through TLSFix's
OpenSSL check and the profile copy). MobileSafari reaches 11.7 MiB resident with
TLSFix after the first page (8.2 MiB without); the system still has 40.5 MiB free
of 128 MiB, so the old `SANDBOX=1` out-of-memory failure does not occur. Numbers
and screenshots: re-log 104.

Turn it off: `make sd TLSFIX=0`, `install.py --no-tlsfix`, `IOS3DS_TLSFIX=0`, or
`"tlsfix": false` in `rootfs/recipe.json`.

## 5. Testing it

`emu/wifi_test.py --https URL ...` (QEMU with the ctr-wifi access point on slirp,
needs internet on the host): the host's test server lists the URLs in its answer
and `ios3ds-nettest` fetches each one through CFNetwork, i.e. with iOS's own TLS
and certificate check, logging one line per URL:

    [nettest] https https://cydia.saurik.com/ -> 200 (TLS ok, 5321 bytes, 9 s)
    [nettest] https https://untrusted-root.badssl.com/ -> SSL error -9807 errSSLXCertChainInvalid: certificate not trusted

On a console, `nettest=https://...` in `wifi.cfg` (docs/wifi.md section 5)
checks one HTTPS URL the same way at boot.
