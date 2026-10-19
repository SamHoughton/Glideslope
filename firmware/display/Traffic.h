#pragma once
/*
Purpose: Live traffic for the map. The main loop hands over one TrafficPoint
per tracked aircraft each fetch; TrafficTracker keeps the latest report
per aircraft and dead-reckons it between fetches.
*/
#include <Arduino.h>
#include <vector>
#include "display/FrameCanvas.h"

struct TrafficPoint
{
    uint32_t id      = 0;      // ICAO24 address
    float    lat     = NAN, lon = NAN;
    float    heading = NAN;    // degrees true
    float    gsKt    = NAN;
    Rgb      colour  {200, 205, 215};
    bool     onFinal = false;
    char     label[8] = "";    // flight number (BA213) or call sign, for the map
    char     type[5]  = "";    // ICAO type code
    float    altFt    = NAN;
};

class TrafficTracker
{
public:
    static constexpr int kMax   = 48;

    // Trail: where it was, one map pixel every kTrailMs, oldest overwritten.
    static constexpr int           kTrail   = 8;
    static constexpr unsigned long kTrailMs = 20000;   // 8 x 20 s: the last 2.7 minutes

    struct Tracked
    {
        TrafficPoint  p;
        unsigned long posMs = 0;           // when p's position was reported
        uint8_t       trail[kTrail][2];    // map x, y
        uint8_t       trailN = 0, trailHead = 0;   // points kept; next slot
        unsigned long trailMs = 0;         // when the last point was added
        // Trail point i, oldest first (i < trailN).
        void trailAt(int i, int &x, int &y) const
        {
            const int k = (trailHead - trailN + i + kTrail) % kTrail;
            x = trail[k][0];
            y = trail[k][1];
        }
    };

    void update(const std::vector<TrafficPoint> &points, unsigned long now);
    const std::vector<Tracked> &aircraft() const { return _tracked; }
    bool empty() const { return _tracked.empty(); }

private:
    std::vector<Tracked> _tracked;
};
