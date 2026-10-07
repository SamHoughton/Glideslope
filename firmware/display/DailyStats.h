#pragma once
/*
Purpose: Today's tally for the stats screen: arrivals seen (aircraft heading
for Heathrow), the busiest airline and the rarest type. Resets at local
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
    // Busiest airline as an IATA/ICAO code plus count; empty if none yet.
    String busiestAirline(int &count) const;
    // Type seen least often today (latest wins a tie); empty if none yet.
    String rarestType() const;

private:
    int                    _day = -1;     // local day of year the counts belong to
    int                    _arrivals = 0;
    std::map<String, bool> _counted;      // idents counted today
    std::map<String, int>  _airlines;
    std::map<String, int>  _types;
    std::map<String, unsigned long> _typeLastMs;

    void rollDay();
};
