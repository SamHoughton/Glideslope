#include "utils/FirmwareSig.h"
#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>
#include <mbedtls/bignum.h>

void FirmwareSig::begin(size_t total)
{
    _total = total;
    _seen = 0;
    mbedtls_sha256_init(&_sha);
    mbedtls_sha256_starts_ret(&_sha, 0);
}

void FirmwareSig::update(const uint8_t *data, size_t n)
{
    // Bytes before the last kTrailer are hashed; the last kTrailer are kept.
    const size_t bodyEnd = _total > kTrailer ? _total - kTrailer : 0;
    for (size_t i = 0; i < n; )
    {
        const size_t pos = _seen + i;
        if (pos < bodyEnd)
        {
            const size_t take = min(n - i, bodyEnd - pos);
            mbedtls_sha256_update_ret(&_sha, data + i, take);
            i += take;
        }
        else
        {
            if (pos - bodyEnd < kTrailer) _trailer[pos - bodyEnd] = data[i];
            ++i;
        }
    }
    _seen += n;
}

FirmwareSig::Result FirmwareSig::finish()
{
    uint8_t hash[32];
    mbedtls_sha256_finish_ret(&_sha, hash);
    mbedtls_sha256_free(&_sha);
    if (_total <= kTrailer || _seen != _total || memcmp(_trailer + 64, "GSSIG1v1", 8) != 0)
        return Unsigned;

    mbedtls_ecp_group grp;
    mbedtls_ecp_point q;
    mbedtls_mpi r, s;
    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&q);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);
    bool ok = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
              mbedtls_ecp_point_read_binary(&grp, &q, FirmwareKey::kPublic, sizeof(FirmwareKey::kPublic)) == 0 &&
              mbedtls_mpi_read_binary(&r, _trailer, 32) == 0 &&
              mbedtls_mpi_read_binary(&s, _trailer + 32, 32) == 0 &&
              mbedtls_ecdsa_verify(&grp, hash, sizeof(hash), &q, &r, &s) == 0;
    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&s);
    mbedtls_ecp_point_free(&q);
    mbedtls_ecp_group_free(&grp);
    return ok ? Signed : Bad;
}
