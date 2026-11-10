#include "utils/Airframes.h"
#include "utils/Storage.h"
#include "utils/TelnetLogger.h"
#include <string.h>
#include <new>
#include <LittleFS.h>
#ifndef NATIVE_TEST
#include <fcntl.h>
#include <unistd.h>
#endif

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
int8_t   s_ready = -1;           // moved (1), still to move (0), not looked yet (-1)

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

Result visit(Store &store, uint32_t icao24, uint16_t today, const char *reg, const char *type, const char *op, bool close)
{
    const Result r = visitOpen(store, icao24, today, reg, type, op);
    if (close) store.done();
    return r;
}

void pathOf(uint32_t file, char *out, size_t len) { snprintf(out, len, "%s/%02u.bin", kDir, (unsigned)file); }

// One of the table's files, open to read and write. On the board, POSIX
// calls on LittleFS's mount: the core's File opens through stdio, which
// takes a 4 KB buffer from the heap at every open (9.0.2 found those
// breaking the heap up). In the tests, the in-memory LittleFS.
#ifdef NATIVE_TEST
struct Handle
{
    File f;
    bool on = false;
    bool open(const char *path)
    {
        close();
        if (!LittleFS.exists(path)) return false;
        f = LittleFS.open(path, "r+");
        return on = (bool)f;
    }
    bool create(const char *path)
    {
        close();
        f = LittleFS.open(path, "w");
        return on = (bool)f;
    }
    bool readAt(uint32_t at, void *buf, size_t n) { return on && f.seek(at) && f.read((uint8_t *)buf, n) == n; }
    bool writeAt(uint32_t at, const void *buf, size_t n) { return on && f.seek(at) && f.write((const uint8_t *)buf, n) == n; }
    void close() { if (on) f.close(); on = false; }
    bool isOpen() const { return on; }
    ~Handle() { close(); }
    static bool truncate(const char *path, uint32_t len) { return LittleFS.truncate(path, len); }
};
#else
struct Handle
{
    int fd = -1;
    static void full(const char *path, char *out, size_t len) { snprintf(out, len, "%s%s", Storage::kMount, path); }
    bool open(const char *path)
    {
        close();
        char p[40];
        full(path, p, sizeof(p));
        fd = ::open(p, O_RDWR);
        return fd >= 0;
    }
    bool create(const char *path)
    {
        close();
        char p[40];
        full(path, p, sizeof(p));
        fd = ::open(p, O_RDWR | O_CREAT | O_TRUNC, 0644);
        return fd >= 0;
    }
    bool readAt(uint32_t at, void *buf, size_t n)
    {
        return fd >= 0 && ::lseek(fd, (off_t)at, SEEK_SET) == (off_t)at && ::read(fd, buf, n) == (ssize_t)n;
    }
    bool writeAt(uint32_t at, const void *buf, size_t n)
    {
        return fd >= 0 && ::lseek(fd, (off_t)at, SEEK_SET) == (off_t)at && ::write(fd, buf, n) == (ssize_t)n;
    }
    void close() { if (fd >= 0) ::close(fd); fd = -1; }
    bool isOpen() const { return fd >= 0; }
    ~Handle() { close(); }
    static bool truncate(const char *path, uint32_t len)
    {
        char p[40];
        full(path, p, sizeof(p));
        return ::truncate(p, (off_t)len) == 0;
    }
};
#endif

namespace
{
// A table file of empty slots (all zeros), ready for one to be written.
bool createEmpty(Handle &h, const char *path, uint32_t bytes)
{
    if (!h.create(path)) return false;
    static const uint8_t kZero[256] = {};
    for (uint32_t at = 0; at < bytes; at += sizeof(kZero))
    {
        const size_t n = bytes - at < sizeof(kZero) ? bytes - at : sizeof(kZero);
        if (!h.writeAt(at, kZero, n))
        {
            h.close();
            LittleFS.remove(path);
            return false;
        }
    }
    return true;
}

struct FileStore : Store
{
    Handle  h;
    int32_t at = -1;   // the file open now

    // The file slot i is in, open; missing: it hasn't been made yet.
    bool openFor(uint32_t i, bool make, bool &missing)
    {
        missing = false;
        const uint32_t file = fileOf(i);
        if (at == (int32_t)file && h.isOpen()) return true;
        at = -1;
        char path[16];
        pathOf(file, path, sizeof(path));
        if (h.open(path)) { at = (int32_t)file; return true; }
        missing = !Storage::exists(path);
        if (!make || !missing) return false;
        if (!Storage::exists(kDir)) LittleFS.mkdir(kDir);
        if (!createEmpty(h, path, slotsIn(file) * sizeof(Slot))) return false;
        at = (int32_t)file;
        return true;
    }
    bool read(uint32_t i, Slot &s) override
    {
        if (!ready() || i >= kSlots) return false;
        bool missing;
        if (!openFor(i, false, missing))
        {
            if (!missing) return false;
            s = Slot();
            return true;
        }
        return h.readAt(offsetOf(i), &s, sizeof(s));
    }
    bool write(uint32_t i, const Slot &s) override
    {
        if (!ready() || i >= kSlots) return false;
        // A slot's change copies its file's block (a new file: a block too).
        if (!Storage::mayWrite(kSlotsPerFile * sizeof(Slot), "the airframe collection")) return false;
        bool missing;
        if (!openFor(i, true, missing)) return false;
        const bool ok = h.writeAt(offsetOf(i), &s, sizeof(s));
        done();   // committed: the block it replaced is free again
        return ok;
    }
    void done() override
    {
        h.close();
        at = -1;
    }
};
}

namespace
{
// At file scope, not a function's static: no guard variable (8 bytes of RAM).
FileStore s_store;
}
Store &fileStore() { return s_store; }

bool needsMove() { return Storage::size(kOldPath) >= 0; }
bool ready()
{
    if (s_ready < 0) s_ready = needsMove() ? 0 : 1;
    return s_ready == 1;
}

bool migrate()
{
    long size = Storage::size(kOldPath);
    s_count = UINT32_MAX;   // counted again when next asked
    if (size < 0) { s_ready = 1; return true; }
    s_ready = 0;
    fileStore().done();
    if (size > (long)(kSlots * sizeof(Slot))) size = (long)(kSlots * sizeof(Slot));
    size -= size % (long)sizeof(Slot);
    Log.printf("Airframes: moving the collection into block-sized files (%ld KB to go)\n", size / 1024);
    if (!Storage::exists(kDir)) LittleFS.mkdir(kDir);
    Handle from, to;
    while (size > 0)
    {
        // The last file's worth still in the old file: copied whole, then
        // cut off it. (Stopped part-way, the old file still holds it, so it
        // is copied again.)
        const uint32_t file = fileOf((uint32_t)(size / sizeof(Slot)) - 1);
        const uint32_t start = file * kSlotsPerFile * sizeof(Slot);
        const uint32_t bytes = (uint32_t)size - start, whole = slotsIn(file) * sizeof(Slot);
        if (!Storage::mayWrite(whole, "the airframe collection's move")) return false;
        char path[16];
        pathOf(file, path, sizeof(path));
        bool ok = from.open(kOldPath) && to.create(path);
        uint8_t buf[256];
        for (uint32_t at = 0; ok && at < bytes; at += sizeof(buf))
        {
            const size_t n = bytes - at < sizeof(buf) ? bytes - at : sizeof(buf);
            ok = from.readAt(start + at, buf, n) && to.writeAt(at, buf, n);
        }
        const Slot empty;
        for (uint32_t at = bytes; ok && at < whole; at += sizeof(Slot)) ok = to.writeAt(at, &empty, sizeof(empty));
        from.close();
        to.close();
        if (!ok || Storage::size(path) != (long)whole || !Handle::truncate(kOldPath, start))
        {
            Log.printf("Airframes: could not move %s; the collection waits\n", path);
            return false;
        }
        size = (long)start;
        delay(1);
    }
    LittleFS.remove(kOldPath);
    Storage::recount();
    s_ready = 1;
    Log.println("Airframes: collection moved");
    return true;
}

Scan::Scan() : _oldSize(Storage::size(kOldPath)) {}
Scan::~Scan() { delete _h; }

size_t Scan::next(Slot *out, size_t n)
{
    if (_i >= kSlots || !n) return 0;
    if (!_h && !(_h = new (std::nothrow) Handle)) return 0;
    // Slots the old file still holds come from it (a move not finished).
    const bool old = _oldSize > 0 && (long)((_i + 1) * sizeof(Slot)) <= _oldSize;
    const int32_t want = old ? -2 : (int32_t)fileOf(_i);
    uint32_t end = old ? (uint32_t)(_oldSize / sizeof(Slot)) : fileOf(_i) * kSlotsPerFile + slotsIn(fileOf(_i));
    if (end > kSlots) end = kSlots;
    const size_t k = end - _i < n ? end - _i : n;
    if (_open != want)
    {
        char path[24];
        if (old) strlcpy(path, kOldPath, sizeof(path));
        else     pathOf((uint32_t)want, path, sizeof(path));
        _missing = !_h->open(path);   // not made yet: empty slots
        _open = want;
    }
    const bool ok = !_missing && _h->readAt(old ? _i * sizeof(Slot) : offsetOf(_i), out, k * sizeof(Slot));
    if (!ok)
        for (size_t j = 0; j < k; ++j) out[j] = Slot();
    _i += (uint32_t)k;
    return k;
}

uint32_t count()
{
    if (s_count != UINT32_MAX) return s_count;
    Scan scan;
    Slot buf[16];
    uint32_t n = 0;
    for (size_t got; (got = scan.next(buf, 16)) > 0;)
        for (size_t i = 0; i < got; ++i) n += buf[i].key != 0;
    s_count = n;
    return n;
}

size_t bytesUsed()
{
    long n = Storage::size(kOldPath);
    size_t total = n > 0 ? (size_t)n : 0;
    for (uint32_t f = 0; f < kFiles; ++f)
    {
        char path[16];
        pathOf(f, path, sizeof(path));
        if ((n = Storage::size(path)) > 0) total += (size_t)n;
    }
    return total;
}
}
