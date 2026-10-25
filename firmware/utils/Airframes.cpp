#include "utils/Airframes.h"
#include <string.h>
#include <LittleFS.h>

namespace Airframes
{
namespace
{
uint32_t hash(uint32_t k)
{
    k ^= k >> 16; k *= 0x7feb352d; k ^= k >> 15; k *= 0x846ca68b; k ^= k >> 16;
    return k;
}
// Fill a fixed field the slot doesn't have yet (empty in the slot, given now).
template <size_t N> bool fill(char (&dst)[N], const char *src)
{
    if (dst[0] || !src || !src[0]) return false;
    strncpy(dst, src, N);
    return true;
}
uint32_t s_count = UINT32_MAX;   // unknown until counted

Result visitOpen(Store &store, uint32_t icao24, uint16_t today, const char *reg, const char *type, const char *op)
{
    const uint32_t key = (icao24 & 0xFFFFFF) + 1;
    const uint32_t start = hash(key) % kSlots;
    int32_t free = -1, oldest = -1;
    uint16_t oldestDay = 0xFFFF;
    for (int p = 0; p < kProbes; ++p)
    {
        const uint32_t i = (start + p) % kSlots;
        Slot s;
        if (!store.read(i, s)) return Failed;
        if (s.key == key)
        {
            const bool filled = fill(s.reg, reg) | fill(s.type, type) | fill(s.op, op);
            if (s.lastDay == today && !filled) return AgainToday;
            if (s.lastDay != today) { s.days = s.days < 0xFFFF ? s.days + 1 : s.days; s.lastDay = today; }
            return store.write(i, s) ? Known : Failed;
        }
        if (s.key == 0) { free = i; break; }   // an empty slot ends the run
        if (s.lastDay < oldestDay) { oldestDay = s.lastDay; oldest = i; }
    }
    const int32_t i = free >= 0 ? free : oldest;
    if (i < 0) return Failed;
    Slot s;
    s.key = key;
    s.firstDay = s.lastDay = today;
    s.days = 1;
    fill(s.reg, reg);
    fill(s.type, type);
    fill(s.op, op);
    if (!store.write((uint32_t)i, s)) return Failed;
    if (free >= 0 && s_count != UINT32_MAX) ++s_count;
    return New;
}
}

Result visit(Store &store, uint32_t icao24, uint16_t today, const char *reg, const char *type, const char *op)
{
    const Result r = visitOpen(store, icao24, today, reg, type, op);
    store.done();
    return r;
}
}

namespace Airframes
{
uint32_t count()
{
    if (s_count != UINT32_MAX) return s_count;
    File f = LittleFS.open(kPath, "r");
    if (!f) return 0;
    uint32_t n = 0;
    Slot chunk[16];
    size_t got;
    while ((got = f.read((uint8_t *)chunk, sizeof(chunk))) >= sizeof(Slot))
        for (size_t i = 0; i < got / sizeof(Slot); ++i)
            if (chunk[i].key) ++n;
    f.close();
    s_count = n;
    return n;
}
}

#ifndef NATIVE_TEST
namespace Airframes
{
namespace
{
struct FileStore : Store
{
    File f;
    bool ready = false;

    bool open()
    {
        if (ready && f) return true;
        if (!LittleFS.exists(kPath))
        {
            // 192 KB of empty slots, written a block at a time (once).
            if (!LittleFS.exists("/log")) LittleFS.mkdir("/log");
            File n = LittleFS.open(kPath, "w");
            if (!n) return false;
            uint8_t zero[256] = {};
            for (uint32_t i = 0; i < kSlots * sizeof(Slot) / sizeof(zero); ++i)
                if (n.write(zero, sizeof(zero)) != sizeof(zero)) { n.close(); LittleFS.remove(kPath); return false; }
            n.close();
            s_count = 0;
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
        return open() && f.seek(i * sizeof(Slot)) && f.write((const uint8_t *)&s, sizeof(Slot)) == sizeof(Slot);
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
