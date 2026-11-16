"""Apple flattened device tree (iBoot 'DeviceTree' format), parse / edit / serialise.

Node:     u32 nProperties, u32 nChildren, properties..., children...
Property: char name[32], u32 length (bit31 = placeholder, filled by iBoot),
          value[length] padded to 4 bytes.
Ref: https://theapplewiki.com/wiki/DeviceTree , xnu pexpert/gen/device_tree.c
"""
import struct

PLACEHOLDER = 0x80000000


class Node:
    def __init__(self):
        self.props = []  # [name, bytes, placeholder_flag]
        self.children = []

    # -- accessors ---------------------------------------------------------
    def get(self, name, default=None):
        for n, v, _ in self.props:
            if n == name:
                return v
        return default

    def set(self, name, value: bytes, placeholder=False):
        for p in self.props:
            if p[0] == name:
                p[1], p[2] = value, placeholder
                return
        self.props.append([name, value, placeholder])

    def delete(self, name):
        self.props = [p for p in self.props if p[0] != name]

    @property
    def name(self):
        v = self.get("name", b"")
        return v.split(b"\0")[0].decode("latin-1")

    def find(self, path):
        node = self
        for part in [p for p in path.split("/") if p]:
            node = next((c for c in node.children if c.name == part), None)
            if node is None:
                return None
        return node

    def walk(self, path=""):
        here = path + "/" + self.name if path or self.name != "device-tree" else ""
        yield here or "/", self
        for c in self.children:
            yield from c.walk(here)


def parse(blob: bytes) -> Node:
    def rd(off):
        n = Node()
        nprops, nchild = struct.unpack_from("<2I", blob, off)
        off += 8
        for _ in range(nprops):
            name = blob[off : off + 32].split(b"\0")[0].decode("latin-1")
            (ln,) = struct.unpack_from("<I", blob, off + 32)
            ph = bool(ln & PLACEHOLDER)
            ln &= ~PLACEHOLDER
            n.props.append([name, blob[off + 36 : off + 36 + ln], ph])
            off += 36 + ((ln + 3) & ~3)
        for _ in range(nchild):
            c, off = rd(off)
            n.children.append(c)
        return n, off

    root, _ = rd(0)
    return root


def serialise(node: Node) -> bytes:
    out = bytearray(struct.pack("<2I", len(node.props), len(node.children)))
    for name, val, ph in node.props:
        nm = name.encode("latin-1")[:31]
        out += nm + b"\0" * (32 - len(nm))
        out += struct.pack("<I", len(val) | (PLACEHOLDER if ph else 0))
        out += val + b"\0" * (((len(val) + 3) & ~3) - len(val))
    for c in node.children:
        out += serialise(c)
    return bytes(out)


def fmt_value(name, v: bytes):
    if v and all(32 <= b < 127 or b == 0 for b in v) and v[:1] != b"\0" and v.rstrip(b"\0"):
        return repr(b"|".join(x for x in v.rstrip(b"\0").split(b"\0")).decode())
    if len(v) % 4 == 0 and 0 < len(v) <= 64:
        return " ".join("%08x" % x for x in struct.unpack("<%dI" % (len(v) // 4), v))
    return v[:32].hex() + ("..(%d bytes)" % len(v) if len(v) > 32 else "")


def dump(root: Node) -> str:
    lines = []
    for path, n in root.walk():
        lines.append(path)
        for name, v, ph in n.props:
            lines.append("    %-28s%s %s" % (name, "[PH]" if ph else "    ", fmt_value(name, v)))
    return "\n".join(lines)
