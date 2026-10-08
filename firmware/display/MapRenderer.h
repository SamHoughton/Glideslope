#pragma once
/*
Purpose: Map mode. The airport's surroundings at 128x64 from a pre-rendered
base layer (Heathrow's in flash: Thames, reservoirs, M25; other airports'
from their airport pack), a home
marker, and every tracked aircraft as a bright dot in its airline colour
as a plane icon. The aircraft on final approach blinks.
*/
#include <Arduino.h>
#include "display/FrameCanvas.h"
#include "display/Traffic.h"

namespace MapRenderer
{
    // Panel coordinates for a position (may be off-panel).
    void project(double lat, double lon, float &x, float &y);

    void render(FrameCanvas &c, const TrafficTracker &traffic, unsigned long now,
                double homeLat, double homeLon, const char *arrivals, const char *departures);
}
