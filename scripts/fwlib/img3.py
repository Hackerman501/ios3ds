"""IMG3 container parsing and decryption (iOS 2.x-4.x, S5L89xx/S5L87xx).

Layout (https://theapplewiki.com/wiki/IMG3_File_Format):
  header: magic 'Img3' (LE u32 0x496d6733), fullSize, sizeNoPack, sigCheckArea, ident
  then tags: magic(u32 LE, ASCII reversed), totalLength, dataLength, data[dataLength], pad
The DATA tag payload is AES-CBC encrypted (key/IV published per build). The
ciphertext is padded to a 16-byte multiple and the padding lives in the tag's
slack (totalLength - 12 - dataLength); dataLength is the plaintext size.
(Verified on iPod2,1 8C148 kernelcache: decrypting only dataLength & ~15 bytes
leaves a 15-byte encrypted tail and LZSS comes up 64 bytes short.)
"""
import struct

from .crypto import aes_cbc_decrypt

IMG3_MAGIC = 0x496D6733  # 'Img3'


def _fourcc(v):
    return struct.pack(">I", v).decode("latin-1")


class Img3:
    def __init__(self, blob: bytes):
        magic, full, nopack, sigarea, ident = struct.unpack_from("<5I", blob, 0)
        if magic != IMG3_MAGIC:
            raise ValueError("not an IMG3 file (magic %08x)" % magic)
        self.ident = _fourcc(ident)
        self.tags = []  # list of (fourcc, data)
        self.padded = {}
        off = 20
        while off + 12 <= min(len(blob), full):
            tmagic, total, dlen = struct.unpack_from("<3I", blob, off)
            if total < 12:
                break
            # keep the padded payload too: encrypted DATA is AES-padded up to a
            # 16-byte multiple inside the tag's padding (dataLength is the real size)
            padded = blob[off + 12 : off + total]
            self.tags.append((_fourcc(tmagic), blob[off + 12 : off + 12 + dlen]))
            self.padded[_fourcc(tmagic)] = padded
            off += total

    def tag(self, name):
        for n, d in self.tags:
            if n == name:
                return d
        return None

    def data(self, key_hex=None, iv_hex=None) -> bytes:
        d = self.tag("DATA")
        if d is None:
            raise ValueError("IMG3 has no DATA tag")
        if not key_hex:
            return d
        key = bytes.fromhex(key_hex)
        iv = bytes.fromhex(iv_hex)
        padded = self.padded["DATA"]
        n = (len(d) + 15) & ~15
        if len(padded) < n:  # no room for padding: tail is plaintext
            n = len(d) & ~15
            return aes_cbc_decrypt(key, iv, d[:n]) + d[n:]
        return aes_cbc_decrypt(key, iv, padded[:n])[: len(d)]
