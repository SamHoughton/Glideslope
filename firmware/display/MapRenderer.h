#pragma once
/*
Purpose: London map mode. West and south-west London at 128x64 from the
pre-rendered base layer in flash (Thames, reservoirs, M25, Heathrow), a home
marker, and every tracked aircraft as a bright dot in its airline colour
with a fading trail. The aircraft on final approach blinks.
*/
#include <Arduino.h>
#include "display/FrameCanvas.h"
#include "display/Traffic.h"

namespace MapRenderer
{
    // Panel coordinates for a position (may be off-panel).
    void project(double lat, double lon, float &x, float &y);

    void render(FrameCanvas &c, const TrafficTracker &traffic, unsigned long now,
                double homeLat, double homeLon, const char *runwayInUse);
}
