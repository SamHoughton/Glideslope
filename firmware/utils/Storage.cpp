#include "utils/Storage.h"
#include "utils/TelnetLogger.h"
#include <LittleFS.h>
#ifndef NATIVE_TEST
#include <sys/stat.h>
#endif

namespace Storage
{
namespace
{
    volatile uint32_t      s_total = 0;       // blocks
    volatile int32_t       s_free = 0;        // blocks: as last counted, less what has been allowed since
    volatile unsigned long s_countedMs = 0;   // 0: count on the next ask
    volatile uint32_t      s_refused = 0;
    volatile bool          s_paused = false;
    unsigned long          s_loggedMs = 0;
#ifdef NATIVE_TEST
    struct Guard { };
#else
    portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
    struct Guard
    {
        Guard() { portENTER_CRITICAL(&s_mux); }
        ~Guard() { portEXIT_CRITICAL(&s_mux); }
    };
#endif

    uint32_t blocksFor(size_t bytes) { return (uint32_t)((bytes + kBlock - 1) / kBlock) + 1; }

    void count()
    {
        // (LittleFS walks the file system here: outside the lock.)
        const size_t total = LittleFS.totalBytes(), used = LittleFS.usedBytes();
        Guard g;
        s_total = (uint32_t)(total / kBlock);
        s_free = total > used ? (int32_t)((total - used) / kBlock) : 0;
        s_countedMs = millis() | 1;
    }

#ifdef NATIVE_TEST
    // The tests' clock stands still: count every time (the stand-in file
    // system counts in no time).
    bool stale(unsigned long) { return true; }
#else
    bool stale(unsigned long maxAgeMs) { return !s_countedMs || millis() - s_countedMs > maxAgeMs; }
#endif
}

bool allowed(uint32_t freeBlocks, size_t bytes) { return freeBlocks >= blocksFor(bytes) + kFloorBlocks; }

bool mayWrite(size_t bytes, const char *what, Who who)
{
    if (s_paused && who == Background) return false;
    if (stale(30000)) count();
    bool ok;
    {
        Guard g;
        ok = allowed(s_free > 0 ? (uint32_t)s_free : 0, bytes);
    }
    // The running estimate may have gone low on its own (it counts every
    // change as new blocks): count again, unless it was just counted.
    if (!ok && stale(2000))
    {
        count();
        Guard g;
        ok = allowed(s_free > 0 ? (uint32_t)s_free : 0, bytes);
    }
    {
        Guard g;
        if (ok) s_free -= (int32_t)blocksFor(bytes);
        else    ++s_refused;
    }
    if (!ok && (!s_loggedMs || millis() - s_loggedMs > 60000))
    {
        s_loggedMs = millis() | 1;
        Log.printf("Storage: %u KB free, not writing %s (%u KB is kept spare)\n",
                   (unsigned)(freeBlocks() * kBlock / 1024), what ? what : "a file", (unsigned)(kFloorBlocks * kBlock / 1024));
    }
    return ok;
}

uint32_t freeBlocks()
{
    if (stale(30000)) count();
    return s_free > 0 ? (uint32_t)s_free : 0;
}

uint32_t totalBlocks()
{
    if (!s_total) count();
    return s_total;
}

bool     low() { return freeBlocks() < kLowBlocks; }
uint32_t refused() { return s_refused; }
void     recount() { s_countedMs = 0; }
void     pause(bool on) { s_paused = on; }
bool     paused() { return s_paused; }

#ifdef NATIVE_TEST
bool exists(const char *path) { return LittleFS.exists(path); }
long size(const char *path)
{
    if (!LittleFS.exists(path)) return -1;
    File f = LittleFS.open(path, "r");
    const long n = (long)f.size();
    f.close();
    return n;
}
#else
namespace
{
    bool statOf(const char *path, struct stat &st)
    {
        char full[48];
        snprintf(full, sizeof(full), "%s%s", kMount, path);
        return ::stat(full, &st) == 0;
    }
}
bool exists(const char *path)
{
    struct stat st;
    return path && statOf(path, st);
}
long size(const char *path)
{
    struct stat st;
    return path && statOf(path, st) ? (long)st.st_size : -1;
}
#endif
}
