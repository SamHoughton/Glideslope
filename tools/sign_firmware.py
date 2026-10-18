#!/usr/bin/env python3
"""
Sign a firmware.bin for the board's update check (utils/FirmwareSig.h):
append an ECDSA P-256 signature over the SHA-256 of the file, then the
magic "GSSIG1v1" (72 bytes in all). GitHub Actions runs this on every
release with the private key from the GLIDESLOPE_SIGNING_KEY secret.

  GLIDESLOPE_SIGNING_KEY="$(cat key.pem)" python tools/sign_firmware.py firmware.bin

Needs the "cryptography" package.
"""
import os
import sys

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature
from cryptography.hazmat.primitives.serialization import load_pem_private_key

MAGIC = b'GSSIG1v1'


def main():
    path = sys.argv[1]
    pem = os.environ.get('GLIDESLOPE_SIGNING_KEY', '').encode()
    if not pem:
        sys.exit('GLIDESLOPE_SIGNING_KEY is not set')
    key = load_pem_private_key(pem, password=None)
    data = open(path, 'rb').read()
    if data.endswith(MAGIC):
        sys.exit(f'{path} is already signed')
    r, s = decode_dss_signature(key.sign(data, ec.ECDSA(hashes.SHA256())))
    open(path, 'wb').write(data + r.to_bytes(32, 'big') + s.to_bytes(32, 'big') + MAGIC)
    print(f'signed {path} ({len(data)} bytes + 72)')


if __name__ == '__main__':
    main()
