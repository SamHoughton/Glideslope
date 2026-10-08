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
};

class TrafficTracker
{
public:
    static constexpr int kMax   = 48;

    struct Tracked
    {
        TrafficPoint  p;
        unsigned long posMs = 0;           // when p's position was reported
    };

    void update(const std::vector<TrafficPoint> &points, unsigned long now);
    const std::vector<Tracked> &aircraft() const { return _tracked; }
    bool empty() const { return _tracked.empty(); }

private:
    std::vector<Tracked> _tracked;
};
