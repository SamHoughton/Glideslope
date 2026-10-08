#include "display/HoldTracker.h"
#include "config/Airport.h"
#include "utils/GeoUtils.h"
#include <math.h>

namespace
{
    constexpr unsigned long kForgetMs     = 150000;   // not seen for this long: gone
    constexpr unsigned long kStackForgetMs = 200000;  // a stack is sampled every 3 minutes
    constexpr unsigned long kStackMinMs   = 150000;   // near a fix on two samples this far apart
    constexpr float         kStackKm      = 9.0f;
    constexpr float         kTurnHold     = 300.0f;   // degrees of sustained turning
    constexpr float         kTurnRelease  = 120.0f;
    constexpr float         kTurnFadeS    = 240.0f;   // turning fades with this time constant

    uint32_t idOf(const StateVector &s) { return (uint32_t)strtoul(s.icao24.c_str(), nullptr, 16) | 1; }

    float feet(const StateVector &s) { return isnan(s.baro_altitude) ? NAN : s.baro_altitude * 3.28084f; }
}

HoldTracker::Entry *HoldTracker::find(uint32_t id, unsigned long now)
{
    Entry *freeSlot = nullptr, *oldest = &_e[0];
    for (Entry &e : _e)
    {
        if (e.id == id) return &e;
        const bool gone = !e.id || (now - e.seenMs > kForgetMs && (!e.stackLastMs || now - e.stackLastMs > kStackForgetMs));
        if (gone) { if (!freeSlot) freeSlot = &e; }
        else if (e.seenMs < oldest->seenMs) oldest = &e;
    }
    Entry *e = freeSlot ? freeSlot : oldest;
    *e = Entry();
    e->id = id;
    return e;
}

void HoldTracker::noteStates(const std::vector<StateVector> &states, unsigned long now)
{
    for (const StateVector &s : states)
    {
        const float ft = feet(s);
        if (s.on_ground || isnan(ft) || ft < 2500 || isnan(s.heading)) continue;
        Entry &e = *find(idOf(s), now);
        const float dt = e.headingMs ? (now - e.headingMs) / 1000.0f : 0;
        if (!isnan(e.heading) && dt > 0 && dt < 40)
        {
            float d = fmodf((float)s.heading - e.heading + 540.0f, 360.0f) - 180.0f;
            e.turn = e.turn * expf(-dt / kTurnFadeS) + d;
        }
        else if (dt >= 40)
            e.turn = 0;   // gap: can't tell how it turned
        e.heading = s.heading;
        e.headingMs = now;
        e.seenMs = now;
        if (!e.turning && fabsf(e.turn) >= kTurnHold)
        {
            e.turning = true;
            if (!e.holdSinceMs) e.holdSinceMs = now - 240000;   // a full turn takes about four minutes
        }
        else if (e.turning && fabsf(e.turn) < kTurnRelease)
        {
            e.turning = false;
            if (!e.stackLastMs) e.holdSinceMs = 0;
        }
        // Near a named stack? (Counts towards that stack's row.)
        if (e.stack < 0)
            for (int i = 0; i < g_airport.holdCount; ++i)
                if (haversineKm(g_airport.holds[i].lat, g_airport.holds[i].lon, s.lat, s.lon) < kStackKm)
                    e.stack = i;
    }
}

void HoldTracker::noteStack(int stack, const std::vector<StateVector> &states, unsigned long now)
{
    if (stack < 0 || stack >= g_airport.holdCount) return;
    const HoldFix &fix = g_airport.holds[stack];
    for (const StateVector &s : states)
    {
        const float ft = feet(s);
        if (s.on_ground || isnan(ft) || ft < 5000 || ft > 20000) continue;
        if (haversineKm(fix.lat, fix.lon, s.lat, s.lon) > kStackKm) continue;
        Entry &e = *find(idOf(s), now);
        if (e.stack != stack || now - e.stackLastMs > kStackForgetMs)
        {
            e.stack = stack;
            e.stackSamples = 0;
            e.stackFirstMs = now;
        }
        if (e.stackSamples < 255) ++e.stackSamples;
        e.stackLastMs = now;
        e.seenMs = max(e.seenMs, now);
        if (e.stackSamples >= 2 && now - e.stackFirstMs >= kStackMinMs && !e.holdSinceMs)
            e.holdSinceMs = e.stackFirstMs;
    }
    // Aircraft of this stack missing from the sample have left it.
    for (Entry &e : _e)
        if (e.id && e.stack == stack && e.stackLastMs && e.stackLastMs != now)
        {
            e.stackSamples = 0;
            e.stackLastMs = 0;
            if (!e.turning) e.holdSinceMs = 0;
        }
}

bool HoldTracker::holding(const Entry &e, unsigned long now) const
{
    const bool atStack = e.id && e.stackLastMs && now - e.stackLastMs <= kStackForgetMs &&
                         e.stackSamples >= 2 && now - e.stackFirstMs >= kStackMinMs;
    if (atStack) return true;
    if (!e.id || now - e.seenMs > kForgetMs) return false;
    return e.turning;
}

int HoldTracker::rows(Row *out, int max, unsigned long now) const
{
    int n = 0;
    const int named = min((int)g_airport.holdCount, max);
    for (int i = 0; i < named; ++i)
    {
        strlcpy(out[n].name, g_airport.holds[i].name, sizeof(out[n].name));
        out[n].count = 0;
        out[n].longestMin = 0;
        ++n;
    }
    Row other{"OTHER", 0, 0};
    for (const Entry &e : _e)
    {
        if (!holding(e, now)) continue;
        Row &r = (e.stack >= 0 && e.stack < named) ? out[e.stack] : other;
        if (r.count < 255) ++r.count;
        const unsigned long mins = e.holdSinceMs ? (now - e.holdSinceMs) / 60000 : 0;
        if (mins > r.longestMin) r.longestMin = (uint8_t)min(mins, 99UL);
    }
    if (other.count && n < max) out[n++] = other;
    return n;
}

int HoldTracker::total(unsigned long now) const
{
    int n = 0;
    for (const Entry &e : _e) if (holding(e, now)) ++n;
    return n;
}
