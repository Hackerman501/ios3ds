#!/usr/bin/env python3
"""ios3ds firmware prep: user-supplied IPSW -> decrypted kernel, device tree, rootfs.

Usage:
  scripts/fwprep.py path/to/iPod2,1_4.2.1_8C148_Restore.ipsw [-o build/staging]
                    [--keys keys.json] [--no-rootfs] [--keep-dmg] [--force]

Nothing produced here may be committed: everything lands in build/ (gitignored).
Keys are the publicly documented per-build firmware keys. They are read from
--keys (ipsw.me v4 'keys' JSON format), else from a cached copy in the output
directory, else fetched once from https://api.ipsw.me/v4/keys/ipsw/<id>/<build>
(the request only contains the device identifier and build number).

Outputs (in the staging dir):
  kernelcache.macho     decrypted + decompressed prelinked kernel (Mach-O)
  kernelcache.txt       segment map, entry point, kext list
  devicetree.bin        decrypted Apple flattened device tree (template)
  devicetree.txt        human-readable dump of it
  rootfs.hfs            raw HFS+ image of the root filesystem (unless --no-rootfs)
  manifest.json         sizes/sha1 of everything + source IPSW identity
"""
import argparse
import hashlib
import json
import os
import plistlib
import re
import sys
import time
import urllib.request
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import versions  # noqa: E402
from fwlib import appledt, udif, vfdecrypt  # noqa: E402
from fwlib.crypto import BACKEND  # noqa: E402
from fwlib.img3 import Img3  # noqa: E402
from fwlib.lzss import unpack_complzss  # noqa: E402
from fwlib.macho import MachO  # noqa: E402

TARGET = {"product": "iPod2,1", "build": "8C148", "version": "4.2.1",
          "ipsw_sha1": "b9efddc7bb4350c237a8d3846af61bbfc8a2f647"}
KEYS_URL = "https://api.ipsw.me/v4/keys/ipsw/{product}/{build}"


def log(msg):
    print("[fwprep] " + msg, flush=True)


def sha1_file(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def progress_printer(label):
    state = {"t": 0}

    def cb(done, total):
        now = time.time()
        if now - state["t"] > 2 or done >= total:
            state["t"] = now
            print("\r[fwprep]   %s %5.1f%% (%d/%d MiB)" % (label, 100.0 * done / max(total, 1),
                  done >> 20, total >> 20), end="" if done < total else "\n", flush=True)
    return cb


def load_keys(args, product, build):
    cache = os.path.join(args.out, "keys-%s-%s.json" % (product, build))
    src = args.keys or (cache if os.path.exists(cache) else None)
    if src:
        log("keys: using %s" % src)
        with open(src) as f:
            data = json.load(f)
    else:
        url = KEYS_URL.format(product=product, build=build)
        log("keys: fetching %s" % url)
        req = urllib.request.Request(url, headers={"User-Agent": "ios3ds-fwprep/1"})
        with urllib.request.urlopen(req, timeout=30) as r:
            data = json.load(r)
        with open(cache, "w") as f:
            json.dump(data, f, indent=1)
    keys = {}
    for k in data.get("keys", []):
        keys.setdefault(k["filename"], k)
    return keys


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ipsw")
    ap.add_argument("-o", "--out", default=os.path.join("build", "staging"))
    ap.add_argument("--keys", help="keys JSON (ipsw.me v4 format) instead of fetching")
    ap.add_argument("--no-rootfs", action="store_true", help="skip the (slow, ~1 GB) root filesystem")
    ap.add_argument("--keep-dmg", action="store_true", help="keep the decrypted UDIF dmg")
    ap.add_argument("--force", action="store_true", help="allow an IPSW other than iPod2,1 8C148")
    ap.add_argument("--ios", default=versions.DEFAULT, help="iOS version (default 4.2.1)")
    args = ap.parse_args()
    want = versions.get(args.ios)
    os.makedirs(args.out, exist_ok=True)
    log("crypto backend: %s" % BACKEND)

    z = zipfile.ZipFile(args.ipsw)
    bm = plistlib.loads(z.read("BuildManifest.plist"))
    product = bm["SupportedProductTypes"][0]
    build = bm["ProductBuildVersion"]
    version = bm["ProductVersion"]
    log("IPSW: %s iOS %s (%s)" % (product, version, build))
    expected = ("iPod2,1", want["build"])
    if (product, build) != expected:
        msg = "expected %s %s for iOS %s" % (expected[0], expected[1], args.ios)
        if not args.force:
            sys.exit("[fwprep] ERROR: " + msg + " (use --force to try anyway)")
        log("WARNING: " + msg)
    if os.path.getsize(args.ipsw) < 1 << 30:
        digest = sha1_file(args.ipsw)
        want_sha1 = want["sha1"] or (TARGET["ipsw_sha1"] if args.ios == versions.DEFAULT else None)
        ok = want_sha1 is not None and digest == want_sha1
        status = "matches Apple's published file" if ok else "UNEXPECTED - continuing"
        if want_sha1 and not ok and not args.force and args.ios != versions.DEFAULT:
            sys.exit("[fwprep] ERROR: IPSW sha1 %s, expected %s for iOS %s (use --force to override)"
                     % (digest, want_sha1, args.ios))
        log("IPSW sha1 %s (%s)" % (digest, status))

    keys = load_keys(args, product, build)
    names = z.namelist()
    manifest = {"source": {"product": product, "version": version, "build": build,
                           "ipsw": os.path.basename(args.ipsw)}, "outputs": {}}

    # --- kernelcache ---------------------------------------------------------
    kname = next(n for n in names if re.match(r"kernelcache\.release\.", n))
    k = keys[kname]
    img = Img3(z.read(kname))
    raw = img.data(k["key"], k["iv"])
    kernel, info = unpack_complzss(raw)
    kpath = os.path.join(args.out, "kernelcache.macho")
    with open(kpath, "wb") as f:
        f.write(kernel)
    mo = MachO(kernel)
    pl = mo.section("__PRELINK_INFO", "__info")
    kexts = []
    if pl:
        xml = kernel[pl["offset"]: pl["offset"] + pl["size"]].decode("latin-1")
        kexts = sorted(set(re.findall(r"<key>CFBundleIdentifier</key><string>([^<]+)</string>", xml)))
    ver = re.search(rb"Darwin Kernel Version [^\0]+", kernel)
    with open(os.path.join(args.out, "kernelcache.txt"), "w") as f:
        f.write((ver.group(0).decode() if ver else "?") + "\n")
        f.write("complzss: %s\n" % json.dumps(info))
        f.write(mo.summary() + "\n\nprelinked kexts (%d):\n" % len(kexts))
        f.write("".join("  %s\n" % x for x in kexts))
    log("kernel: %s, entry %#x, %d kexts, %d bytes" % (
        ver.group(0).decode().split(";")[-1].strip() if ver else "?", mo.entry, len(kexts), len(kernel)))
    manifest["kernel"] = {"version": ver.group(0).decode() if ver else None, "entry": mo.entry,
                          "uuid": mo.uuid, "text_vmaddr": mo.segment("__TEXT")["vmaddr"]}

    # --- device tree ---------------------------------------------------------
    dname = next(n for n in names if re.search(r"/DeviceTree\.[^/]+\.img3$", n))
    d = keys[dname]
    dt = Img3(z.read(dname)).data(d["key"], d["iv"])
    root = appledt.parse(dt)
    if appledt.serialise(root) != dt[: len(appledt.serialise(root))]:
        sys.exit("[fwprep] ERROR: device tree did not round-trip; parser bug")
    with open(os.path.join(args.out, "devicetree.bin"), "wb") as f:
        f.write(dt)
    with open(os.path.join(args.out, "devicetree.txt"), "w") as f:
        f.write(appledt.dump(root) + "\n")
    log("devicetree: %d bytes, %d nodes, round-trip OK" % (len(dt), sum(1 for _ in root.walk())))

    # --- root filesystem -----------------------------------------------------
    if not args.no_rootfs:
        rk = next(v for n, v in keys.items() if v["image"] == "RootFS")
        rname = rk["filename"]
        dmg = os.path.join(args.out, "rootfs.udif.dmg")
        log("rootfs: decrypting %s (%d MiB)" % (rname, z.getinfo(rname).file_size >> 20))
        with z.open(rname) as fin, open(dmg, "wb") as fout:
            vfdecrypt.decrypt_stream(fin, fout, rk["key"], progress_printer("decrypt"))
        hfs = os.path.join(args.out, "rootfs.hfs")
        with open(dmg, "rb") as f:
            parts = udif.partitions(f)
            for p in parts:
                log("  udif partition: %-40s first=%d sectors=%d" % (p["name"], p["first"], p["count"]))
            target = [p for p in parts if "Apple_HFS" in p["name"]]
            if len(target) != 1:
                sys.exit("[fwprep] ERROR: expected exactly one Apple_HFS partition")
            with open(hfs, "wb") as fout:
                udif.extract(f, fout, target[0], progress_printer("inflate"))
        with open(hfs, "rb") as f:
            f.seek(1024)
            sig = f.read(2)
        if sig not in (b"H+", b"HX"):
            sys.exit("[fwprep] ERROR: rootfs.hfs has no HFS+ signature (got %r)" % sig)
        log("rootfs: HFS+ volume OK (%s), %d MiB" % (sig.decode(), os.path.getsize(hfs) >> 20))
        if not args.keep_dmg:
            os.remove(dmg)

    for fn in sorted(os.listdir(args.out)):
        p = os.path.join(args.out, fn)
        if os.path.isfile(p) and not fn.startswith("keys-") and fn != "manifest.json":
            manifest["outputs"][fn] = {"size": os.path.getsize(p), "sha1": sha1_file(p)}
    with open(os.path.join(args.out, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=1)
    log("done -> %s" % args.out)


if __name__ == "__main__":
    main()
