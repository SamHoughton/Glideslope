#include "utils/SeenLog.h"
#include <time.h>

namespace SeenLog
{
namespace
{
uint32_t hash(uint32_t k)
{
    k ^= k >> 16; k *= 0x7feb352d; k ^= k >> 15; k *= 0x846ca68b; k ^= k >> 16;
    return k;
}
}

namespace
{
bool visitOpen(Store &store, uint32_t icao24, uint16_t today, Slot &before);
}

bool visit(Store &store, uint32_t icao24, uint16_t today, Slot &before)
{
    const bool ok = visitOpen(store, icao24, today, before);
    store.done();
    return ok;
}

namespace
{
bool visitOpen(Store &store, uint32_t icao24, uint16_t today, Slot &before)
{
    before = Slot{};
    const uint32_t key = (icao24 & 0xFFFFFF) + 1;
    const uint32_t start = hash(key) % kSlots;
    int32_t free = -1, oldest = -1;
    uint16_t oldestDay = 0xFFFF;
    for (int p = 0; p < kProbes; ++p)
    {
        const uint32_t i = (start + p) % kSlots;
        Slot s;
        if (!store.read(i, s)) return false;
        if (s.key == key)
        {
            before = s;
            if (s.lastDay == today) return true;   // already counted today
            s.days = s.days < 0xFFFF ? s.days + 1 : s.days;
            s.lastDay = today;
            return store.write(i, s);
        }
        if (s.key == 0) { if (free < 0) free = i; break; }   // an empty slot ends the run
        if (s.lastDay < oldestDay) { oldestDay = s.lastDay; oldest = i; }
    }
    // New aircraft: the first empty slot, else the one seen longest ago.
    const int32_t i = free >= 0 ? free : oldest;
    if (i < 0) return false;
    Slot s;
    s.key = key;
    s.days = 1;
    s.lastDay = today;
    return store.write((uint32_t)i, s);
}
}

uint16_t dayNumber(time_t t)
{
    struct tm lt;
    localtime_r(&t, &lt);
    // Days from 1 Jan 2020 to the local date (civil-from-days, Howard Hinnant).
    int y = lt.tm_year + 1900, m = lt.tm_mon + 1;
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + lt.tm_mday - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long days = era * 146097L + (long)doe - 719468L;   // since 1 Jan 1970
    const long d2020 = days - 18262;                          // 1 Jan 2020
    return d2020 < 0 ? 0 : (uint16_t)d2020;
}
}

#ifndef NATIVE_TEST
#include <LittleFS.h>

namespace SeenLog
{
namespace
{
constexpr const char *kPath = "/seen.bin";

struct FileStore : Store
{
    File f;
    bool ready = false;

    bool open()
    {
        if (ready && f) return true;
        if (!LittleFS.exists(kPath))
        {
            // 16 KB of empty slots, written a block at a time.
            File n = LittleFS.open(kPath, "w");
            if (!n) return false;
            uint8_t zero[256] = {};
            for (uint32_t i = 0; i < kSlots * sizeof(Slot) / sizeof(zero); ++i)
                if (n.write(zero, sizeof(zero)) != sizeof(zero)) { n.close(); LittleFS.remove(kPath); return false; }
            n.close();
        }
        f = LittleFS.open(kPath, "r+");
        ready = (bool)f && f.size() == kSlots * sizeof(Slot);
        return ready;
    }
    bool read(uint32_t i, Slot &s) override
    {
        return open() && f.seek(i * sizeof(Slot)) && f.read((uint8_t *)&s, sizeof(Slot)) == sizeof(Slot);
    }
    bool write(uint32_t i, const Slot &s) override
    {
        if (!open() || !f.seek(i * sizeof(Slot)) || f.write((const uint8_t *)&s, sizeof(Slot)) != sizeof(Slot))
            return false;
        return true;
    }
    // Closed between visits: an open file keeps a cache buffer in RAM.
    void done() override
    {
        if (f) f.close();
        ready = false;
    }
};
}

Store &fileStore()
{
    static FileStore s;
    return s;
}
}
#endif
