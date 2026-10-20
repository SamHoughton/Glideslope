#include "config/Airports.h"
#include "config/AirportDb.h"
#include "config/AirportTz.h"

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
    out.index = (uint16_t)(&e - AirportDb::kTable);
}

// Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm).
long daysFromCivil(int y, int m, int d)
{
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}

// The day of the month of the n-th Sunday (n = -1: the last) of year y, month m.
int sunday(int y, int m, int n)
{
    if (n > 0)
    {
        const int wd = (int)((daysFromCivil(y, m, 1) + 4) % 7);   // 0 = Sunday (1970-01-01 was a Thursday)
        return 1 + (7 - wd) % 7 + 7 * (n - 1);
    }
    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const int last = kDays[m - 1] + (m == 2 && ((y % 4 == 0 && y % 100) || y % 400 == 0));
    const int wd = (int)((daysFromCivil(y, m, last) + 4) % 7);
    return last - wd;
}

// Unix time of hh:00 UTC on that day.
time_t utcAt(int y, int m, int d, int hh) { return (time_t)(daysFromCivil(y, m, d) * 86400L + hh * 3600L); }

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

int utcOffsetMinutes(const Place &p, time_t t)
{
    if (p.index >= AirportDb::kCount) return 0;
    const int std = ((int)AirportTz::kOffset[p.index] - 64) * 15;
    const char region = AirportTz::kRegion[p.index];
    if (region == 'N' || t < 1600000000) return std;
    const int y = (int)(1970 + (t / 86400L) / 365.2425);   // close enough: the boundaries are months away
    const int sh = std / 60;                               // standard hours, for "02:00 local"
    bool dst = false;
    switch (region)
    {
        case 'E':   // last Sunday of March to last Sunday of October, 01:00 UTC
            dst = t >= utcAt(y, 3, sunday(y, 3, -1), 1) && t < utcAt(y, 10, sunday(y, 10, -1), 1);
            break;
        case 'A':   // second Sunday of March to first Sunday of November, 02:00 local
            dst = t >= utcAt(y, 3, sunday(y, 3, 2), 2 - sh) && t < utcAt(y, 11, sunday(y, 11, 1), 1 - sh);
            break;
        case 'O':   // southern: first Sunday of October to first Sunday of April
            dst = t >= utcAt(y, 10, sunday(y, 10, 1), 2 - sh) || t < utcAt(y, 4, sunday(y, 4, 1), 2 - sh);
            break;
        case 'Z':   // last Sunday of September to first Sunday of April
            dst = t >= utcAt(y, 9, sunday(y, 9, -1), 2 - sh) || t < utcAt(y, 4, sunday(y, 4, 1), 2 - sh);
            break;
        case 'S':   // Chile and Paraguay: early September to early April
            dst = t >= utcAt(y, 9, sunday(y, 9, 1), 4 - sh) || t < utcAt(y, 4, sunday(y, 4, 1), 3 - sh);
            break;
    }
    return std + (dst ? 60 : 0);
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
