#include "display/RunwayTracker.h"
#include "display/ApproachModel.h"
#include "config/Airport.h"
#include "utils/TelnetLogger.h"
#include <math.h>

namespace
{
    constexpr unsigned long kActiveMs  = 10UL * 60 * 1000;   // in use if seen this recently
    constexpr unsigned long kQuietMs   = 30UL * 60 * 1000;   // no arrivals this long: no runway
    constexpr float         kHalfLifeS = 300.0f;             // scores halve every 5 minutes

    float angleDiff(float a, float b)
    {
        float d = fmodf(a - b + 540.0f, 360.0f) - 180.0f;
        return fabsf(d);
    }

    void remember(uint32_t ids[4], uint32_t id)
    {
        for (int i = 0; i < 4; ++i) if (ids[i] == id) return;
        for (int i = 3; i > 0; --i) ids[i] = ids[i - 1];
        ids[0] = id;
    }
}

int RunwayTracker::distinct(const uint32_t ids[4])
{
    int n = 0;
    for (int i = 0; i < 4; ++i) if (ids[i]) ++n;
    return n;
}

bool RunwayTracker::update(const std::vector<StateVector> &states, unsigned long now)
{
    // Fade every score by the time since the last update.
    if (_lastMs)
    {
        const float k = powf(0.5f, (now - _lastMs) / 1000.0f / kHalfLifeS);
        for (Slot &s : _slots) { s.arr *= k; s.dep *= k; }
    }
    _lastMs = now;

    const int n = min((int)g_airport.runwayCount, kMax);
    for (const StateVector &sv : states)
    {
        FlightInfo f;   // position only: no route needed
        f.lat = sv.lat;  f.lon = sv.lon;  f.heading = sv.heading;
        f.baro_altitude = isnan(sv.baro_altitude) ? NAN : sv.baro_altitude * 3.28084;
        f.velocity      = isnan(sv.velocity) ? NAN : sv.velocity * 1.94384;
        f.vertical_rate = isnan(sv.vertical_rate) ? NAN : sv.vertical_rate * 196.85;
        if (isnan(f.baro_altitude) || f.baro_altitude > 5000) continue;
        const uint32_t id = (uint32_t)strtoul(sv.icao24.c_str(), nullptr, 16) | 1;

        char rwy[4] = "";
        bool arrival = false;
        const ApproachStatus st = ApproachModel::evaluate(f);
        if ((st.phase == ApproachStatus::Approach || st.phase == ApproachStatus::Landing) &&
            !isnan(st.distKm) && st.distKm < 15.0f)
        {
            strlcpy(rwy, st.runway, sizeof(rwy));
            arrival = true;
        }
        else if (!ApproachModel::climbingOut(f, rwy, sizeof(rwy)))
            continue;

        for (int i = 0; i < n; ++i)
            if (strcmp(g_airport.runways[i].name, rwy) == 0)
            {
                Slot &s = _slots[i];
                if (arrival)
                {
                    if (s.arrMs && now - s.arrMs > kActiveMs) memset(s.arrIds, 0, sizeof(s.arrIds));
                    s.arr += 1; s.arrMs = now; remember(s.arrIds, id);
                }
                else
                {
                    if (s.depMs && now - s.depMs > kActiveMs) memset(s.depIds, 0, sizeof(s.depIds));
                    s.dep += 1; s.depMs = now; remember(s.depIds, id);
                }
                break;
            }
    }

    // The main arrival runway: the busiest one seen recently.
    int best = -1;
    for (int i = 0; i < n; ++i)
    {
        const Slot &s = _slots[i];
        if (!s.arrMs || now - s.arrMs > kActiveMs) continue;
        if (best < 0 || s.arr > _slots[best].arr) best = i;
    }
    // Nothing landing for a while (overnight): no runway in use.
    bool anyRecent = false;
    for (int i = 0; i < n; ++i) if (_slots[i].arrMs && now - _slots[i].arrMs < kQuietMs) anyRecent = true;
    if (!anyRecent) { _main[0] = '\0'; return false; }
    if (best < 0) return false;

    const RunwayEnd &r = g_airport.runways[best];
    if (strcmp(r.name, _main) == 0) return false;

    // A different main runway. A new landing direction needs two aircraft
    // to believe it; a parallel swap (27L <-> 27R) is just noted.
    const bool newDirection = !isnan(_mainCourse) && angleDiff(r.course, _mainCourse) > 90.0f;
    if (newDirection && distinct(_slots[best].arrIds) < 2) return false;

    strlcpy(_from, _main, sizeof(_from));
    strlcpy(_main, r.name, sizeof(_main));
    const bool announce = newDirection && _from[0];
    _mainCourse = r.course;
    if (announce)
        Log.printf("Runways: landing direction changed, %s -> %s\n", _from, _main);
    else if (_from[0])
        Log.printf("Runways: now landing on %s (was %s)\n", _main, _from);
    else
        Log.printf("Runways: landing on %s\n", _main);
    return announce;
}

void RunwayTracker::list(bool arrivalsWanted, char *out, size_t len) const
{
    if (!len) return;
    out[0] = '\0';
    const int n = min((int)g_airport.runwayCount, kMax);
    bool used[kMax] = {};
    for (int k = 0; k < 3; ++k)   // at most three, busiest first
    {
        int best = -1;
        for (int i = 0; i < n; ++i)
        {
            if (used[i]) continue;
            const Slot &s = _slots[i];
            const unsigned long ms = arrivalsWanted ? s.arrMs : s.depMs;
            const float score = arrivalsWanted ? s.arr : s.dep;
            if (!ms || _lastMs - ms > kActiveMs || score < 0.5f) continue;
            if (best < 0 || score > (arrivalsWanted ? _slots[best].arr : _slots[best].dep)) best = i;
        }
        if (best < 0) break;
        used[best] = true;
        if (out[0]) strlcat(out, " ", len);
        strlcat(out, g_airport.runways[best].name, len);
    }
}

void RunwayTracker::arrivals(char *out, size_t len) const   { list(true, out, len); }
void RunwayTracker::departures(char *out, size_t len) const { list(false, out, len); }
