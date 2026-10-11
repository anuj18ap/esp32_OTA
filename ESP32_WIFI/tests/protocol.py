"""Wire helpers shared by laptop transports; no hardware assumed."""

import base64
import hashlib
import hmac
import json
import secrets
import struct
import time
import zlib
from pathlib import Path

MASK = 0xFFFFFFFF
KEY = (0xA6E31F4B, 0xC904B27D, 0x51D8A3E9, 0x7B2F90C5)


def _ctr(data, hi, lo):
    out = bytearray(data)
    for counter, offset in enumerate(range(0, len(out), 8)):
        a, b, total = hi ^ counter, (lo + counter) & MASK, 0
        for _ in range(32):
            a = (a + ((((b << 4) ^ (b >> 5)) + b) ^ (total + KEY[total & 3]))) & MASK
            total = (total + 0x9E3779B9) & MASK
            b = (b + ((((a << 4) ^ (a >> 5)) + a) ^ (total + KEY[(total >> 11) & 3]))) & MASK
        stream = struct.pack(">II", a, b)
        for i in range(min(8, len(out) - offset)):
            out[offset + i] ^= stream[i]
    return bytes(out)


def encrypt(text, nonce=None):
    nonce = nonce or secrets.token_bytes(8)
    hi, lo = struct.unpack(">II", nonce)
    return b"ENC1:" + base64.b64encode(nonce + _ctr(text.encode(), hi, lo))


def decrypt(data):
    if not data.startswith(b"ENC1:"):
        return data.decode()
    raw = base64.b64decode(data[5:], validate=True)
    if len(raw) < 8:
        raise ValueError("short encrypted envelope")
    return _ctr(raw[8:], *struct.unpack(">II", raw[:8])).decode()


def metadata(path, chunk_size):
    data = Path(path).read_bytes()
    if not data:
        raise ValueError("empty firmware")
    return {
        "size": len(data),
        "chunks": (len(data) + chunk_size - 1) // chunk_size,
        "crc32": zlib.crc32(data) & MASK,
        "sha256": hashlib.sha256(data).hexdigest(),
        "chip": "esp32",
    }


def signed_request(fields, key):
    request = dict(fields)
    request["nonce"] = time.time_ns() // 1000
    names = ("op", "nonce", "size", "chunks", "crc32", "sha256", "version", "url", "session_id")
    canonical = "|".join(
        str(
            request.get(
                name, 0 if name in ("nonce", "size", "chunks", "crc32", "session_id") else ""
            )
        )
        for name in names
    )
    request["auth"] = hmac.new(bytes.fromhex(key), canonical.encode(), hashlib.sha256).hexdigest()
    return encrypt(json.dumps(request, separators=(",", ":")))
