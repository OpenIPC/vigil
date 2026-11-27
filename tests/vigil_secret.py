# SPDX-License-Identifier: GPL-3.0-or-later
"""Open the passwords vigild seals in its files (src/core/secret.h), for
the end-to-end tests: "enc:v1:" + base64(nonce[12] | ciphertext | tag[16]),
AES-256-GCM under the key in vigil.key, the field's path the additional
data. Needs the cryptography package (python3-cryptography); without it
HAVE is False and the callers skip what needs it."""
import base64
import re

try:
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    HAVE = True
except ImportError:  # pragma: no cover
    HAVE = False

PREFIX = "enc:v1:"
SEALED = re.compile(r"enc:v1:[A-Za-z0-9+/=]+")


def read_key(path):
    with open(path) as f:
        return bytes.fromhex(f.read().strip())


def open_value(key, aad, value):
    """The sealed `value` of field `aad` opened; raises if it does not open."""
    assert value.startswith(PREFIX), value
    raw = base64.b64decode(value[len(PREFIX):])
    return AESGCM(key).decrypt(raw[:12], raw[12:], aad.encode()).decode()


def open_url(key, aad, url):
    """A URL whose password is sealed, with the password itself."""
    m = SEALED.search(url)
    return url if not m else url[:m.start()] + open_value(key, aad, m.group(0)) + url[m.end():]
