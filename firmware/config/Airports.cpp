#include "config/Airports.h"
#include "config/AirportDb.h"

namespace Airports
{
namespace
{
void fill(const AirportDb::Entry &e, Place &out)
{
    memcpy(out.icao, e.icao, 4);
    out.icao[4] = '\0';
    memcpy(out.iata, e.iata, 3);
    out.iata[3] = '\0';
    out.lat  = e.lat100 / 100.0f;
    out.lon  = e.lon100 / 100.0f;
    out.city = AirportDb::kCities + 2 * (size_t)e.city;
}

// Codes compare as fixed-width, case-insensitive (the table is upper case).
int cmp(const char *key, const char *code, int n)
{
    for (int i = 0; i < n; ++i)
    {
        const char k = (char)toupper((unsigned char)key[i]);
        if (k != code[i]) return (unsigned char)k < (unsigned char)code[i] ? -1 : 1;
        if (!k) return -1;
    }
    return 0;
}
}

bool byIcao(const char *icao, Place &out)
{
    if (!icao || strlen(icao) != 4) return false;
    int lo = 0, hi = (int)AirportDb::kCount - 1;
    while (lo <= hi)
    {
        const int mid = (lo + hi) / 2;
        const int c   = cmp(icao, AirportDb::kTable[mid].icao, 4);
        if (c == 0) { fill(AirportDb::kTable[mid], out); return true; }
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return false;
}

bool byIata(const char *iata, Place &out)
{
    if (!iata || strlen(iata) != 3) return false;
    int lo = 0, hi = (int)AirportDb::kCount - 1;
    while (lo <= hi)
    {
        const int mid = (lo + hi) / 2;
        const AirportDb::Entry &e = AirportDb::kTable[AirportDb::kByIata[mid]];
        const int c = cmp(iata, e.iata, 3);
        if (c == 0) { fill(e, out); return true; }
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return false;
}

bool find(const AirportInfo &a, Place &out)
{
    return byIcao(a.code_icao.c_str(), out) || byIata(a.code_iata.c_str(), out);
}

const char *city(const AirportInfo &a)
{
    Place p;
    return find(a, p) ? p.city : nullptr;
}

const char *city(const char *code)
{
    Place p;
    if (!code) return nullptr;
    const size_t n = strlen(code);
    return (n == 4 ? byIcao(code, p) : n == 3 && byIata(code, p)) ? p.city : nullptr;
}

bool shortCity(const char *code, size_t maxChars, char *out, size_t len)
{
    const char *c = city(code);
    if (!c || !len) return false;
    size_t n = 0;
    if (strlen(c) <= maxChars)
        n = snprintf(out, len, "%s", c);
    else
    {
        for (const char *p = c; *p && n + 1 < len && n < maxChars; ++p)
            if (*p != ' ') out[n++] = *p;
        out[n] = '\0';
        // Spaces dropped only when that makes it fit; otherwise cut as is.
        size_t letters = 0;
        for (const char *p = c; *p; ++p) letters += *p != ' ';
        if (letters > maxChars) n = snprintf(out, len, "%.*s", (int)maxChars, c);
    }
    while (n && out[n - 1] == ' ') out[--n] = '\0';
    return true;
}

bool icaoToIata(const char *icao, char outIata[4])
{
    Place p;
    if (byIcao(icao, p)) { memcpy(outIata, p.iata, 4); return true; }
    outIata[0] = '\0';
    return false;
}
}
