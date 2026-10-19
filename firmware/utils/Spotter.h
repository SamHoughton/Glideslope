#pragma once
/*
Purpose: The spotter's alarms, from every aircraft in range on each fetch:
 - the watchlist: registrations, call signs, aircraft types or transponder
   addresses the owner wants to know about (a trailing * matches a prefix:
   "B74*", "RRR*");
 - overhead: an aircraft passing within overhead_km of the home marker. By
   default only overflights; arrivals and departures at the airport (which
   get their own cards) count only with overhead_all.
Each aircraft is announced once per cooldown (watchlist 6 hours, overhead
45 minutes). Pure logic, no network: main.cpp shows and sends the result.
*/
#include <Arduino.h>
#include <vector>
#include "models/StateVector.h"

namespace Spotter
{
struct Settings
{
    double home_lat = 0, home_lon = 0;   // 0, 0: no home marker (no overhead)
    float  overhead_km = 0;              // 0: off
    bool   overhead_all = false;         // arrivals and departures too
    const char *watchlist = "";          // comma or space separated
};

struct Event
{
    int   index = -1;       // into the states
    bool  watch = false;    // watchlist (else overhead)
    char  matched[16] = ""; // the watchlist entry that matched
    float distKm = NAN;     // from home
};

// The most notable new event this fetch, if any: a watchlist match first,
// then the closest overhead. nowMs drives the cooldowns.
bool check(const std::vector<StateVector> &states, const Settings &cfg, unsigned long nowMs, Event &out);

// Whether s matches an entry of list; the entry is copied to matched.
bool watchMatch(const char *list, const StateVector &s, char *matched, size_t len);

// Forget the cooldowns (tests).
void reset();
}
