#pragma once
/*
Purpose: Today's tally for the stats screen: arrivals and departures, by
hour, the busiest airline, the rarest type and go-arounds.

Counted from every tracked aircraft (not just the ones that get a card, which
with "nearest only" is a small fraction): an arrival once it is on final for
a runway within 12 km of the threshold, a departure once it is climbing out
along one. Neither needs a route lookup. Each movement is counted once, by a
32-bit hash of its transponder address, call sign and direction, in a fixed
table.

Saved to /stats.bin on LittleFS every few minutes, so a restart keeps the
day's count; resets at local midnight.
*/
#include <Arduino.h>
#include <vector>
#include "models/StateVector.h"

class DailyStats
{
public:
    // Every aircraft in range, once per fetch.
    void noteTraffic(const std::vector<StateVector> &states);
    void noteGoAround() { rollDay(); ++_goArounds; _dirty = true; }

    int  arrivals() const { return _arrivals; }
    int  departures() const { return _departures; }
    int  goArounds() const { return _goArounds; }
    // Movements in each local hour today (0-23).
    int  arrivalsInHour(int h) const { return (h >= 0 && h < 24) ? _arrHour[h] : 0; }
    int  departuresInHour(int h) const { return (h >= 0 && h < 24) ? _depHour[h] : 0; }
    // Busiest airline as an IATA code (or ICAO) plus count; empty if none yet.
    String busiestAirline(int &count) const;
    // Type seen least often today (the latest wins a tie); empty if none yet.
    String rarestType() const;

    // Yesterday, for comparison: movements in each hour, and by this time of day.
    bool hasYesterday() const { return _hasYesterday; }
    int  yesterdayInHour(int h) const { return (h >= 0 && h < 24) ? _yHour[h] : 0; }
    int  yesterdayUpTo(int hour, int minute) const;

    // Persistence (LittleFS must be mounted). serialise() is quick and is
    // called under the display lock; the file write happens outside it.
    bool dirty() const { return _dirty; }
    void serialise(std::vector<uint8_t> &out);
    static bool writeFile(const std::vector<uint8_t> &data);
    void load();

private:
    int      _year = -1, _day = -1;   // local date the counts belong to
    uint16_t _arrivals = 0, _departures = 0, _goArounds = 0;
    uint16_t _arrHour[24] = {}, _depHour[24] = {};
    uint16_t _yHour[24] = {};          // yesterday, arrivals + departures
    bool     _hasYesterday = false;
    bool     _dirty = false;

    static constexpr size_t kSeenSlots = 1536;   // 6 KB; up to 1,152 movements a day
    uint32_t _seen[kSeenSlots] = {};
    uint16_t _seenCount = 0;
    bool     markSeen(uint32_t h);    // false if already counted
    bool     wasSeen(uint32_t h) const;

    // Fixed tables, not maps: a std::map<String> node costs ~50 bytes of heap,
    // and over a day the tallies grew to ~8 KB, below the TLS headroom.
    struct AirlineCount { char code[4]; uint16_t n; };
    struct TypeCount    { char t[6]; uint16_t n; uint16_t seq; };
    static constexpr size_t kMaxAirlines = 48, kMaxTypes = 80;
    AirlineCount _airlines[kMaxAirlines] = {};
    TypeCount    _types[kMaxTypes] = {};
    uint8_t      _nAirlines = 0, _nTypes = 0;
    uint16_t     _seq = 0;
    void addAirline(const char *code, uint16_t n);
    TypeCount *typeSlot(const char *t, bool create);

    void rollDay();
    void reset();
    void count(const StateVector &s, bool arrival, int hour);
};
