#pragma once
/*
Purpose: Registration history: how many days each airframe has been on the
board, and when it was last seen ("SEEN 14 TIMES", "FIRST VISIT").

Kept in flash, not RAM: /seen.bin is a hash table of 2,048 eight-byte slots
(16 KB) keyed by the 24-bit transponder address, read and written one slot
at a time. A visit counts once a day, so a slot is written at most daily
per aircraft. When the probe run for a new aircraft is full, the slot seen
longest ago is reused.
*/
#include <Arduino.h>

namespace SeenLog
{
struct Slot
{
    uint32_t key = 0;       // transponder address + 1; 0 = empty
    uint16_t days = 0;      // days seen
    uint16_t lastDay = 0;   // days since 1 Jan 2020 when last seen
};
static constexpr uint32_t kSlots = 2048;
static constexpr int      kProbes = 12;

// Slot storage: the file on the board, an array in the tests.
struct Store
{
    virtual bool read(uint32_t i, Slot &s) = 0;
    virtual bool write(uint32_t i, const Slot &s) = 0;
    virtual void done() {}   // after each visit (the file is closed again)
    virtual ~Store() = default;
};

// Record a sighting of icao24 on day `today` and return what was known
// before it (days = 0: first visit). False if the store failed.
bool visit(Store &store, uint32_t icao24, uint16_t today, Slot &before);

// Days since 1 Jan 2020 for a Unix time (local date).
uint16_t dayNumber(time_t t);

// The board's store, on LittleFS (mounted): created empty on first use.
Store &fileStore();
}
