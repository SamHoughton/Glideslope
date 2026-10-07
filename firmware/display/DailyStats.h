#pragma once
/*
Purpose: Today's tally for the stats screen: arrivals and departures seen,
the busiest airline and the rarest type (across both). Resets at local
midnight. Kept in RAM, so a reboot starts the day's count again.
*/
#include <Arduino.h>
#include <map>
#include "models/FlightInfo.h"

class DailyStats
{
public:
    // Count an arrival once per flight per day (call for every fetched flight).
    void note(const FlightInfo &f);

    int  arrivals() const { return _arrivals; }
    int  departures() const { return _departures; }
    void noteGoAround() { rollDay(); ++_goArounds; }
    int  goArounds() const { return _goArounds; }
    // Busiest airline as an IATA/ICAO code plus count; empty if none yet.
    String busiestAirline(int &count) const;
    // Type seen least often today (latest wins a tie); empty if none yet.
    String rarestType() const;

private:
    int                    _day = -1;     // local day of year the counts belong to
    int                    _arrivals = 0;
    int                    _departures = 0;
    int                    _goArounds = 0;
    // Flights counted today, as 32-bit hashes of the ident in a fixed
    // open-addressing table (a String map grew by ~80 bytes per flight all
    // day long). 0 marks an empty slot.
    static constexpr size_t kSeenSlots = 1024;   // 4 KB; up to 768 flights a day
    uint32_t               _seen[kSeenSlots] = {};
    size_t                 _seenCount = 0;
    bool                   markSeen(const String &ident);   // false if already counted
    std::map<String, int>  _airlines;
    std::map<String, int>  _types;
    std::map<String, unsigned long> _typeLastMs;

    void rollDay();
};
