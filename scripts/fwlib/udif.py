"""UDIF (.dmg) -> raw image converter (UDZO/UDRO/UDBZ, raw, zero-fill chunks).

Format: 512-byte 'koly' trailer (big-endian) -> XML plist at XMLOffset ->
resource-fork/blkx[] each with base64 'mish' block tables. Ref:
http://newosxbook.com/DMG.html and dmg2img / xpwn dmg/.
"""
import bz2
import plistlib
import struct
import zlib

SECTOR = 512
T_ZERO, T_RAW, T_IGNORE = 0x00000000, 0x00000001, 0x00000002
T_ADC, T_ZLIB, T_BZ2, T_LZFSE = 0x80000004, 0x80000005, 0x80000006, 0x80000007
T_COMMENT, T_END = 0x7FFFFFFE, 0xFFFFFFFF


def read_koly(f):
    f.seek(-512, 2)
    k = f.read(512)
    if k[:4] != b"koly":
        raise ValueError("no UDIF koly trailer")
    data_off, data_len = struct.unpack_from(">QQ", k, 24)
    xml_off, xml_len = struct.unpack_from(">QQ", k, 216)
    (sectors,) = struct.unpack_from(">Q", k, 492)
    return {"data_off": data_off, "data_len": data_len, "xml_off": xml_off,
            "xml_len": xml_len, "sectors": sectors}


def partitions(f):
    k = read_koly(f)
    f.seek(k["xml_off"])
    pl = plistlib.loads(f.read(k["xml_len"]))
    parts = []
    for e in pl["resource-fork"]["blkx"]:
        mish = e["Data"]
        if mish[:4] != b"mish":
            continue
        first, count, data_off = struct.unpack_from(">QQQ", mish, 8)
        (nchunks,) = struct.unpack_from(">I", mish, 200)
        chunks = [struct.unpack_from(">IIQQQQ", mish, 204 + 40 * i) for i in range(nchunks)]
        parts.append({"name": e.get("Name") or e.get("CFName", ""), "first": first,
                      "count": count, "data_off": k["data_off"] + data_off, "chunks": chunks})
    return parts


def extract(f, fout, part, progress=None):
    """Write one partition (blkx) as a raw image starting at its first sector."""
    total = part["count"] * SECTOR
    for typ, _c, sect, scount, coff, clen in part["chunks"]:
        if typ in (T_COMMENT, T_END):
            continue
        out_off = sect * SECTOR
        out_len = scount * SECTOR
        if typ in (T_ZERO, T_IGNORE):
            fout.seek(out_off + out_len - 1)
            fout.write(b"\0")
            continue
        f.seek(part["data_off"] + coff)
        src = f.read(clen)
        if typ == T_RAW:
            data = src
        elif typ == T_ZLIB:
            data = zlib.decompress(src)
        elif typ == T_BZ2:
            data = bz2.decompress(src)
        else:
            raise ValueError("unsupported UDIF chunk type %#x (ADC/LZFSE not implemented)" % typ)
        fout.seek(out_off)
        fout.write(data[:out_len])
        if progress:
            progress(out_off + out_len, total)
    fout.truncate(total)
    return total
