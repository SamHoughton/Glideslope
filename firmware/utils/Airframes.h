#pragma once
/*
Purpose: Every airframe the board has seen (9.0): its registration, type and
airline, the day it was first and last seen, and on how many days. Every
aircraft in range with a registration counts, not just the one on the card.
For the sticker book's collection and fleet tallies.

Kept in flash, not RAM: /log/airframes.bin is a hash table of 8,192
24-byte slots (192 KB) keyed by the 24-bit transponder address, like
utils/SeenLog's: a probe or two per new aircraft, a write at most once a
day per aircraft. A collection is for keeping, so a full probe run reuses
the slot seen longest ago only after several thousand airframes.
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
Result visit(Store &store, uint32_t icao24, uint16_t today, const char *reg, const char *type, const char *op);

constexpr const char *kPath = "/log/airframes.bin";
// How many airframes are in the collection (counted from the file once,
// then kept up to date by visit()).
uint32_t count();
#ifndef NATIVE_TEST
Store &fileStore();
#endif
}
