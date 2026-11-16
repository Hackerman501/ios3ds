#!/usr/bin/env python3
"""List prelinked kexts in the staged kernelcache: load address, size, and the
IOKit personalities (class, provider, name/compatible match).

  scripts/kexts.py [filter]
"""
import os
import plistlib
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fwlib.macho import MachO  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load_prelink(blob):
    mo = MachO(blob)
    s = mo.section("__PRELINK_INFO", "__info")
    xml = blob[s["offset"]: s["offset"] + s["size"]].rstrip(b"\0")
    # Apple's prelink XML uses ID/IDREF attributes plistlib doesn't know: expand IDREFs
    ids = {}
    for m in re.finditer(rb'<(integer|string)([^>]*) ID="(\d+)"([^>]*)>([^<]*)</\1>', xml):
        ids[m.group(3)] = (m.group(1), m.group(5))
    def rep(m):
        kind, val = ids[m.group(2)]
        return b"<%s>%s</%s>" % (kind, val, kind)
    xml = re.sub(rb'<(integer|string)[^>]* IDREF="(\d+)"\s*/>', rep, xml)
    xml = re.sub(rb' (ID|size)="[^"]*"', b"", xml)
    if not xml.lstrip().startswith((b"<?xml", b"<plist")):
        xml = b"<plist version=\"1.0\">" + xml + b"</plist>"
    return plistlib.loads(xml)


def main():
    blob = open(os.path.join(ROOT, "build", "staging", "kernelcache.macho"), "rb").read()
    info = load_prelink(blob)
    flt = sys.argv[1].lower() if len(sys.argv) > 1 else ""
    for k in info["_PrelinkInfoDictionary"]:
        bid = k.get("CFBundleIdentifier", "?")
        addr = k.get("_PrelinkExecutableLoadAddr", 0)
        size = k.get("_PrelinkExecutableSize", 0)
        pers = k.get("IOKitPersonalities", {})
        line = "%-55s %#010x %#8x" % (bid, addr, size)
        if flt and flt not in line.lower() and flt not in str(pers).lower():
            continue
        print(line)
        for name, p in pers.items():
            print("    %-30s class=%s provider=%s match=%s" % (
                name, p.get("IOClass"), p.get("IOProviderClass"),
                p.get("IONameMatch") or p.get("IOPropertyMatch") or p.get("IOResourceMatch") or ""))


if __name__ == "__main__":
    main()
