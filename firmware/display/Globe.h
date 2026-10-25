#pragma once
/*
Purpose: The route globe: an orthographic Earth turned to face a flight's
route, land from a 1-degree mask (display/LandMask.h), the night side shaded
from the real sun, the great circle from origin to destination (solid where
the aircraft has been, dashed ahead) and the aircraft on it; beside it the
flight, the two cities, a progress bar, distance (in turn, 8.4: how long it
has been in the air) and ETA.
Memory: drawn straight into the frame, nothing kept between frames.
*/
#include <Arduino.h>
#include <time.h>
#include "display/FrameCanvas.h"

namespace Globe
{
struct Route
{
    bool   valid = false;
    char   ident[10] = "";
    Rgb    accent{255, 185, 60};
    char   from[12] = "", to[12] = "";   // city names (or codes) as shown
    float  oLat = 0, oLon = 0, dLat = 0, dLon = 0;
    float  lat = NAN, lon = NAN;         // the aircraft now; NAN if unknown
    bool   landed = false;
    time_t landedAt = 0;                 // Unix time, 0 if unknown
    time_t eta = 0;                      // expected arrival, 0 to work it out from the speed
    float  speedKts = NAN;               // ground speed
};

// shownMs: how long the screen has been up (the globe turns in, the route draws on).
// now: Unix time, for the sun (anything before 2020 leaves the globe fully lit).
void render(FrameCanvas &c, const Route &r, uint32_t shownMs, time_t now);

// Roughly how long a flight has been in the air (minutes), from the
// distance flown: a typical average for the trip's length, plus 20 minutes
// for the climb and the descent. ADS-B carries no take-off time.
int flyingMinutes(float flownKm, float tripKm);

// Where the sun is overhead at time t.
void subsolar(time_t t, float &lat, float &lon);
}
