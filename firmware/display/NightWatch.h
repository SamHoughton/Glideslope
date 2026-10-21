#pragma once
/*
Purpose: Night watch (8.1): through the night hours, instead of cards and
scenes, a quiet view out of the window. The night scene (skyline, the real
sky and moon, the time), and every tracked aircraft as just its lights,
where it really is: across the panel by its compass bearing from home
relative to the way the window faces (60 degrees either side), up it by its
angle above the horizon. A steady white light, the nav light on the side
you would see (red moving left, green moving right) and a slow red beacon,
brighter the closer it is. A dim tracker along the ground: tonight's
movements and the last, the next expected and today's total.
*/
#include <Arduino.h>
#include "display/FrameCanvas.h"
#include "display/Sky.h"
#include "display/Traffic.h"

namespace NightWatch
{
    struct Tracker
    {
        int    tonight = 0;      // movements since night watch began
        time_t last = 0;         // the last movement (0: none yet)
        time_t next = 0;         // the next expected arrival (0: none known)
        int    today = 0;        // the day's movements
    };

    void render(FrameCanvas &c, unsigned long now, const Sky::Look &sky, const char *weather,
                const TrafficTracker &traffic, double homeLat, double homeLon, float facingDeg,
                const Tracker &t);
}
