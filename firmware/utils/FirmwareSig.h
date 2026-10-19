#pragma once
/*
Purpose: Check that an uploaded firmware.bin is a signed Glideslope release.

GitHub Actions appends a 72-byte trailer to every release's firmware.bin
(tools/sign_firmware.py): an ECDSA P-256 signature (r||s, 64 bytes) over the
SHA-256 of everything before it, then the magic "GSSIG1v1". The board hashes
the image as it streams in and checks the signature against the public key
below. The trailer is written to flash with the image and ignored there (the
image's own header gives its length).

  Signed    a release, untouched since GitHub built it
  Unsigned  no trailer: someone's own build; installs only after the web
            page has asked (X-Glideslope-Unsigned: 1)
  Bad       a trailer that doesn't verify: refused

The private key lives only in the repository's GLIDESLOPE_SIGNING_KEY
Actions secret. To change keys, release one build signed with the old key
that carries the new public key.
*/
#include <Arduino.h>
#include <mbedtls/sha256.h>

class FirmwareSig
{
public:
    enum Result : uint8_t { Signed, Unsigned, Bad };
    static constexpr size_t kTrailer = 72;

    void begin(size_t total);
    void update(const uint8_t *data, size_t n);
    Result finish();

private:
    mbedtls_sha256_context _sha;
    size_t  _total = 0, _seen = 0;
    uint8_t _trailer[kTrailer];
};

namespace FirmwareKey
{
    // P-256 public key, uncompressed (0x04 || X || Y).
    static const uint8_t kPublic[65] = {
        0x04, 0xd5, 0x45, 0x19, 0x05, 0x91, 0xb1, 0x91, 0x05, 0xc3, 0x20, 0x76, 0x25,
        0xe8, 0x7e, 0x9b, 0xa2, 0x6b, 0xa2, 0xcb, 0xbf, 0x24, 0x27, 0x78, 0xa1, 0x41,
        0x5f, 0x3f, 0x63, 0x9e, 0x65, 0x3e, 0x08, 0x20, 0x67, 0x82, 0x37, 0x3c, 0x30,
        0xae, 0x11, 0xdb, 0x9a, 0x6c, 0x97, 0x14, 0x45, 0x46, 0xea, 0x42, 0xa2, 0x60,
        0x77, 0x28, 0xec, 0x1e, 0xb8, 0xe2, 0xc9, 0x24, 0x48, 0x81, 0xfa, 0xc4, 0xad,
    };
}
