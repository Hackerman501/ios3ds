"""Apply the kernel patch recipes (kernel/patches.json) to the flat kernel image.

Each patch: {"name", "va", "orig": "hex bytes", "new": "hex bytes", "why",
             "enabled": true}
'orig' must match exactly, or the build stops: a different kernelcache can
never be silently mis-patched. Hex strings are in memory byte order (e.g.
"00f020e3" is the ARM word 0xe320f000). 'new' may contain @sym+0xOFF@ tokens,
replaced by the little-endian word sym+OFF (symbols come from mkboot, e.g. the
shim's virtual address).
"""
import json
import re
import struct

TOKEN = re.compile(r"@([a-z_]+)\+0x([0-9a-fA-F]+)@")


def _expand(hexstr, symbols):
    def rep(m):
        if m.group(1) not in symbols:
            raise KeyError("unknown patch symbol %s" % m.group(1))
        return struct.pack("<I", symbols[m.group(1)] + int(m.group(2), 16)).hex()
    return TOKEN.sub(rep, hexstr.replace(" ", ""))


def apply_all(img, base_va, path, symbols=None):
    with open(path) as f:
        recipes = json.load(f)
    applied = []
    for p in recipes:
        if not p.get("enabled", True):
            continue
        va = int(p["va"], 16) if isinstance(p["va"], str) else p["va"]
        orig = bytes.fromhex(p["orig"].replace(" ", ""))
        new = bytes.fromhex(_expand(p["new"], symbols or {}))
        if len(orig) != len(new):
            raise ValueError("patch %s: orig/new length differ" % p["name"])
        off = va - base_va
        cur = bytes(img[off:off + len(orig)])
        if cur != orig:
            raise ValueError("patch %s @%#x: expected %s, found %s (wrong kernelcache?)"
                             % (p["name"], va, orig.hex(), cur.hex()))
        img[off:off + len(new)] = new
        applied.append({"name": p["name"], "va": va, "why": p.get("why", "")})
    return applied
