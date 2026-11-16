"""Apple 'complzss' kernelcache container + Okumura LZSS (N=4096, F=18, THRESHOLD=2).

Header (big-endian), see xnu libkern/c++/OSKext / BootX lzss.c and
https://theapplewiki.com/wiki/Kernelcache :
  0x00 'comp'  0x04 'lzss'  0x08 adler32  0x0C uncompressed size  0x10 compressed size
  0x14 0x16C bytes padding  -> compressed data at 0x180
"""
import struct
import zlib

N, F, THRESHOLD = 4096, 18, 2


def lzss_decompress(src: bytes, out_size: int) -> bytes:
    text = bytearray(b" " * (N - F) + b"\0" * F)
    r = N - F
    dst = bytearray()
    i, n = 0, len(src)
    flags = 0
    while True:
        flags >>= 1
        if not flags & 0x100:
            if i >= n:
                break
            flags = src[i] | 0xFF00
            i += 1
        if flags & 1:
            if i >= n:
                break
            c = src[i]
            i += 1
            dst.append(c)
            text[r] = c
            r = (r + 1) & (N - 1)
        else:
            if i + 1 >= n:
                break
            a, b = src[i], src[i + 1]
            i += 2
            pos = a | ((b & 0xF0) << 4)
            length = (b & 0x0F) + THRESHOLD + 1
            for k in range(length):
                c = text[(pos + k) & (N - 1)]
                dst.append(c)
                text[r] = c
                r = (r + 1) & (N - 1)
        if len(dst) >= out_size:
            break
    return bytes(dst[:out_size])


def unpack_complzss(blob: bytes):
    """Returns (payload, info dict). Raises ValueError if checksum/size mismatch."""
    sig, kind, adler, usize, csize = struct.unpack_from(">4s4sIII", blob, 0)
    if sig != b"comp" or kind != b"lzss":
        raise ValueError("not a complzss container (got %r %r)" % (sig, kind))
    payload = lzss_decompress(blob[0x180 : 0x180 + csize], usize)
    calc = zlib.adler32(payload) & 0xFFFFFFFF
    info = {"compressed": csize, "uncompressed": usize, "adler32": "%08x" % adler,
            "adler32_ok": calc == adler, "size_ok": len(payload) == usize}
    if not (info["adler32_ok"] and info["size_ok"]):
        raise ValueError("complzss verification failed: %r (calc adler %08x)" % (info, calc))
    return payload, info
