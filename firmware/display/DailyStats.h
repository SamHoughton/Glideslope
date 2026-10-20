#pragma once
/*
Purpose: Today's tally for the stats screen: arrivals and departures, by
hour, the busiest airline, the rarest type and go-arounds; the runway
rhythm (how often they land, the tightest gap, each runway's share) and the
spotter log (today's interesting sightings, 7.1).

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
    // The busiest airlines / types today, most first: fills up to cap rows
    // and returns how many. Distinct counts for the page titles.
    struct Top { char code[7]; int n; };
    int topAirlines(Top *out, int cap) const;
    int topTypes(Top *out, int cap) const;
    int airlineCount() const { return _nAirlines; }
    int typeCount() const { return _nTypes; }

    // ── Runway rhythm (7.1) ────────────────────────────────────────────────
    // Arrivals are timed as they join final (12 km out), which spaces them
    // as they land. A gap over 15 minutes is a lull, not a gap.
    // Seconds between the recent arrivals (the last eight gaps); 0 = not enough yet.
    int  recentGapSec() const;
    // Average gap today, and the tightest (with the second aircraft and when).
    int  averageGapSec() const { return _gapN ? (int)(_gapSum / _gapN) : 0; }
    int  tightestGapSec() const { return _tightSec; }
    const char *tightestIdent() const { return _tightId; }
    int  tightestAtMinute() const { return _tightMin; }   // local minutes since midnight
    // Arrivals by runway, most first (up to four runways).
    struct RunwayCount { char name[4]; uint16_t n; };
    int  runways(RunwayCount *out, int cap) const;
    // The busiest hour so far (both directions), -1 if none.
    int  busiestHour(int &movements) const;

    // ── Spotter log (7.1) ──────────────────────────────────────────────────
    enum SpotKind : uint8_t { Rare, FirstVisit, Watchlist, Overhead, GoAround, Emergency };
    struct Spot { uint16_t minute; uint8_t kind; char ident[8]; char what[9]; };
    static constexpr int kMaxSpots = 16;
    // Adds a sighting (the same aircraft and kind only once a day, except
    // go-arounds and emergencies); the oldest drops off when full.
    void noteSpot(SpotKind kind, const char *ident, const char *what);
    int  spotCount() const { return _nSpots; }
    // i = 0 is the newest.
    const Spot &spot(int i) const { return _spots[(_spotHead + kMaxSpots - 1 - i) % kMaxSpots]; }
    int  spotsToday() const { return _spotsToday; }   // all of them, not just the 16 kept

    // Yesterday, for comparison: movements in each hour, and by this time of day.
    bool hasYesterday() const { return _hasYesterday; }
    int  yesterdayInHour(int h) const { return (h >= 0 && h < 24) ? _yHour[h] : 0; }
    int  yesterdayUpTo(int hour, int minute) const;

    // Persistence (LittleFS must be mounted). serialise() is quick and is
    // called under the display lock; the file write happens outside it.
    bool dirty() const { return _dirty; }
    void serialise(std::vector<uint8_t> &out);
    size_t serialisedSize() const;   // exactly what serialise() writes (~7 KB)
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

    // Runway rhythm.
    uint32_t    _lastArr = 0;          // Unix time of the last arrival
    uint32_t    _gapSum = 0;
    uint16_t    _gapN = 0, _tightSec = 0, _tightMin = 0;
    char        _tightId[8] = "";
    uint16_t    _recent[8] = {};
    uint8_t     _nRecent = 0, _nRwy = 0;
    RunwayCount _rwy[4] = {};
    void noteArrival(const StateVector &s, const char *runway);

    // Spotter log: a ring, newest at _spotHead - 1.
    Spot     _spots[kMaxSpots] = {};
    uint8_t  _nSpots = 0, _spotHead = 0;
    uint16_t _spotsToday = 0;
    // What serialise() writes for the two above, magic included.
    static constexpr size_t kExtraSize = 4 + 4 + 4 + 2 + 2 + 2 + 8 + 16 + 1 + 1 + sizeof(RunwayCount) * 4 +
                                         sizeof(Spot) * kMaxSpots + 1 + 1 + 2;

    void rollDay();
    void reset();
    void count(const StateVector &s, bool arrival, int hour);
};
