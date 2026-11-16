#!/usr/bin/env python3
"""Build a 3DS FIRM image (boot9strap / Luma3DS chainloader format).

  scripts/mkfirm.py out.firm --arm9-entry 0x08000040 --arm11-entry 0x1FF80000 \
      --section 0x08000040:arm9.bin --section 0x1FF80000:arm11.bin [--screen-init]

Header (boot9strap stage2/arm9/source/firm.h, TuxSH/firmtool):
  0x000 'FIRM' | 0x004 boot priority | 0x008 ARM11 entry | 0x00C ARM9 entry
  0x010 reserved[0x30] (byte 0x10: bit0 = screen init, bit1 = skip bootrom lockout)
  0x040 4 x {u32 offset, u32 load address, u32 size, u32 copy method, u8 sha256[32]}
  0x100 RSA-2048 signature (not checked by Luma's chainloader; left zero)
Luma's checkFirm() requires: section offsets >= 0x200 and 512-aligned, sizes
512-aligned, sha256 correct, each entry point inside a section, and sections
only inside 0x08000000-0x08100000, 0x18000000-0x18600000, 0x1FF00000-0x1FFFFC00,
0x20000000-0x28000000 (and not overlapping Luma's buffer at 0x20001000).
"""
import argparse
import hashlib
import struct

ALLOWED = [(0x08000000, 0x08100000), (0x18000000, 0x18600000),
           (0x1FF00000, 0x1FFFFC00), (0x20000000, 0x28000000)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--arm9-entry", type=lambda x: int(x, 0), required=True)
    ap.add_argument("--arm11-entry", type=lambda x: int(x, 0), default=0)
    ap.add_argument("--section", action="append", default=[], help="LOADADDR:file (max 4)")
    ap.add_argument("--screen-init", action="store_true", help="ask the loader to init the LCDs")
    a = ap.parse_args()
    if not 1 <= len(a.section) <= 4:
        ap.error("need 1-4 sections")
    hdr = bytearray(0x200)
    struct.pack_into("<4sIII", hdr, 0, b"FIRM", 0, a.arm11_entry, a.arm9_entry)
    hdr[0x10] = 1 if a.screen_init else 0
    body = bytearray()
    entries = {a.arm9_entry: False, a.arm11_entry: a.arm11_entry == 0}
    for i, spec in enumerate(a.section):
        addr_s, path = spec.split(":", 1)
        addr = int(addr_s, 0)
        data = open(path, "rb").read()
        data += b"\0" * (-len(data) % 0x200)
        if not any(lo <= addr and addr + len(data) <= hi for lo, hi in ALLOWED):
            raise SystemExit("section %d at %#x+%#x is outside Luma's allowed ranges" % (i, addr, len(data)))
        for e in entries:
            if addr <= e < addr + len(data):
                entries[e] = True
        off = 0x200 + len(body)
        struct.pack_into("<IIII32s", hdr, 0x40 + 0x30 * i, off, addr, len(data), 2,
                         hashlib.sha256(data).digest())
        body += data
    # Luma reads the whole .firm file to 0x20001000 before copying sections out
    firm_end = 0x20001000 + 0x200 + len(body)
    for i in range(len(a.section)):
        _o, addr, size = struct.unpack_from("<III", hdr, 0x40 + 0x30 * i)
        if addr < firm_end and addr + size > 0x20001000:
            raise SystemExit("section %d overlaps Luma's FIRM buffer 0x20001000-%#x" % (i, firm_end))
    missing = [hex(e) for e, ok in entries.items() if not ok]
    if missing:
        raise SystemExit("entry point(s) %s not inside any section" % ", ".join(missing))
    with open(a.out, "wb") as f:
        f.write(hdr + body)
    print("wrote %s (%d bytes, %d sections, screen-init=%d)" % (a.out, len(hdr) + len(body), len(a.section), hdr[0x10]))


if __name__ == "__main__":
    main()
