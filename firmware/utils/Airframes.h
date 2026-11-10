#pragma once
/*
Purpose: Every airframe the board has seen (9.0): its registration, type and
airline, the day it was first and last seen, and on how many days. Every
aircraft in range with a registration counts, not just the one on the card.
For the sticker book's collection and fleet tallies.

Kept in flash, not RAM: a hash table of 8,192 24-byte slots (192 KB) keyed
by the 24-bit transponder address, like utils/SeenLog's: a probe or two per
new aircraft, a write at most once a day per aircraft. A collection is for
keeping, so a full probe run reuses the slot seen longest ago only after
several thousand airframes.

9.2.1: the table is 49 files of one flash block each (/af/00.bin to
/af/48.bin, 170 slots apiece), not one 192 KB file. LittleFS copies
everything after a change in a file to new blocks before it lets the old
ones go, so a slot written near the start of the big file took up to 48
free blocks, and a batch of them took them twice over: the flash ran out
and the framework's LittleFS crashes when it does (utils/Storage.h). With
the same aircraft still in range after the restart, it crashed again a
minute later. A slot written now copies its own block, nothing more.
*/
#include <Arduino.h>

namespace Airframes
{
struct Slot
{
    uint32_t key = 0;                        // transponder address + 1; 0 = empty
    uint16_t firstDay = 0, lastDay = 0;      // days since 1 Jan 2020
    uint16_t days = 0;
    char     reg[7] = {}, type[4] = {}, op[3] = {};   // not terminated when full
};
static_assert(sizeof(Slot) == 24, "Airframes::Slot is 24 bytes on disk");
static constexpr uint32_t kSlots = 8192;
static constexpr int      kProbes = 16;

// The files: one flash block each. Slot i is in file fileOf(i) at offsetOf(i).
static constexpr uint32_t kSlotsPerFile = 4096 / sizeof(Slot);                     // 170
static constexpr uint32_t kFiles = (kSlots + kSlotsPerFile - 1) / kSlotsPerFile;   // 49
inline uint32_t fileOf(uint32_t i) { return i / kSlotsPerFile; }
inline uint32_t offsetOf(uint32_t i) { return (i % kSlotsPerFile) * sizeof(Slot); }
inline uint32_t slotsIn(uint32_t file) { return file + 1 < kFiles ? kSlotsPerFile : kSlots - file * kSlotsPerFile; }
constexpr const char *kDir = "/af";
void pathOf(uint32_t file, char *out, size_t len);   // "/af/07.bin"
// The single file the table was until 9.2.1 (the same slots in order).
constexpr const char *kOldPath = "/log/airframes.bin";

struct Store
{
    virtual bool read(uint32_t i, Slot &s) = 0;
    virtual bool write(uint32_t i, const Slot &s) = 0;
    virtual void done() {}
    virtual ~Store() = default;
};

enum Result : uint8_t { Failed, Known, AgainToday, New };
// Note a sighting; New the first time this airframe is ever seen. reg, type
// and op (the call sign's airline) fill in any the slot doesn't have yet.
// close false: more visits follow, call store.done() after the last.
Result visit(Store &store, uint32_t icao24, uint16_t today, const char *reg, const char *type, const char *op,
             bool close = true);

// The table in its files: a missing file is empty slots (made when one of
// its slots is first written); each write is closed straight away, so it
// is committed and the block it replaced is free again. Asks
// utils/Storage before every write.
Store &fileStore();

// From the single file to the block-sized ones (9.2.1), at the start: the
// old file is copied a file's worth at a time from its end, and each piece
// cut off it once copied, so the move needs a block or two of free space,
// not 192 KB, and carries on where it stopped after a power cut. Until it
// has finished (no room yet, or safe mode) the collection pauses: nothing
// is written, and the old file is still read.
bool migrate();
bool needsMove();   // the old file is still there
bool ready();       // moved, or nothing to move

// Every slot in order, a few at a time, wherever it is kept (the CSV, the
// year's summary, the count).
struct Handle;
class Scan
{
public:
    Scan();
    ~Scan();
    // Up to n slots, never across the end of a file; 0 at the end.
    size_t next(Slot *out, size_t n);

private:
    Scan(const Scan &) = delete;
    Scan &operator=(const Scan &) = delete;
    Handle  *_h = nullptr;
    uint32_t _i = 0;
    long     _oldSize = -1;
    int32_t  _open = -1;      // file open now: -2 the old one
    bool     _missing = false;
};

// How many airframes are in the collection (counted once, then kept up to
// date by visit()), and the bytes the table takes.
uint32_t count();
size_t   bytesUsed();
}
