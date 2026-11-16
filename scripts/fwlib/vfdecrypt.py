"""Decrypt an 'encrcdsa' (FileVault v2) iOS root filesystem DMG with a known VFDecrypt key.

Mirrors vfdecrypt.c (planetbeing/xpwn, ipsw-patch/vfdecrypt) for the v2 header:
  big-endian header: 'encrcdsa', version=2, enc_iv_size, unk1..5, uuid[16],
  blocksize (u32 @0x34), datasize (u64 @0x38), dataoffset (u64 @0x40)
Key = 36 bytes: AES-128 key (16) || HMAC-SHA1 key (20).
Chunk i (blocksize bytes, normally 4096) is AES-128-CBC with
  IV = HMAC-SHA1(hmac_key, be32(i))[:16].
"""
import struct

from .crypto import aes_cbc_decrypt, hmac_sha1


def decrypt_stream(fin, fout, key_hex: str, progress=None) -> int:
    key = bytes.fromhex(key_hex)
    if len(key) != 36:
        raise ValueError("VFDecrypt key must be 36 bytes (72 hex chars), got %d" % len(key))
    aes_key, hmac_key = key[:16], key[16:]
    hdr = fin.read(0x48)
    if hdr[:8] != b"encrcdsa":
        raise ValueError("not an encrcdsa v2 image (magic %r)" % hdr[:8])
    (version,) = struct.unpack_from(">I", hdr, 8)
    if version != 2:
        raise ValueError("unsupported encrcdsa version %d" % version)
    (blocksize,) = struct.unpack_from(">I", hdr, 0x34)
    datasize, dataoffset = struct.unpack_from(">QQ", hdr, 0x38)
    # skip to the data (the stream may not be seekable: zip member)
    to_skip = dataoffset - 0x48
    while to_skip > 0:
        n = len(fin.read(min(to_skip, 1 << 20)))
        if n == 0:
            raise ValueError("truncated header")
        to_skip -= n
    done, chunk = 0, 0
    batch = 256  # chunks per write
    while done < datasize:
        out = bytearray()
        for _ in range(batch):
            if done + len(out) >= datasize:
                break
            c = fin.read(blocksize)
            if not c:
                raise ValueError("truncated image at chunk %d" % chunk)
            iv = hmac_sha1(hmac_key, struct.pack(">I", chunk))[:16]
            out += aes_cbc_decrypt(aes_key, iv, c[: len(c) & ~15])
            chunk += 1
        keep = min(len(out), datasize - done)
        fout.write(out[:keep])
        done += keep
        if progress:
            progress(done, datasize)
    return done
