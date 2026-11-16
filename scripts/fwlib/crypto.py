"""Tiny crypto shim: python 'cryptography' if present, else the openssl CLI."""
import hashlib
import hmac
import shutil
import subprocess

try:
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

    def aes_cbc_decrypt(key: bytes, iv: bytes, data: bytes) -> bytes:
        d = Cipher(algorithms.AES(key), modes.CBC(iv)).decryptor()
        return d.update(data) + d.finalize()

    def aes_cbc_decryptor(key: bytes, iv: bytes):
        return Cipher(algorithms.AES(key), modes.CBC(iv)).decryptor()

    BACKEND = "cryptography"
except ImportError:  # pragma: no cover - fallback path
    if not shutil.which("openssl"):
        raise SystemExit("need either the python 'cryptography' package or the openssl CLI")

    def aes_cbc_decrypt(key: bytes, iv: bytes, data: bytes) -> bytes:
        cipher = {16: "aes-128-cbc", 24: "aes-192-cbc", 32: "aes-256-cbc"}[len(key)]
        return subprocess.run(
            ["openssl", "enc", "-d", "-" + cipher, "-nopad", "-K", key.hex(), "-iv", iv.hex()],
            input=data, capture_output=True, check=True).stdout

    aes_cbc_decryptor = None
    BACKEND = "openssl-cli"


def hmac_sha1(key: bytes, msg: bytes) -> bytes:
    return hmac.new(key, msg, hashlib.sha1).digest()
