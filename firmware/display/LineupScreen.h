#pragma once
/*
Purpose: The approach line-up (8.0): the glide path seen side on, the
threshold on the left and about 27 km out on the right, every aircraft on
final at its real distance and height as a half-size sprite in its airline
colour, its flight number beside it, and the time to the one ahead under the
ground line (red under a minute).

Fed a short list (nearest first is not required; it is sorted here), so the
board, the web page's demo and the desktop renders all draw it the same way.
*/
#include <Arduino.h>
#include "display/FrameCanvas.h"
#include "display/Sky.h"
#include "display/Traffic.h"

namespace LineupScreen
{
    struct Plane
    {
        char  id[8]   = "";
        char  type[5] = "";
        Rgb   colour{200, 205, 215};
        float km      = NAN;   // to the threshold, now
        float altFt   = NAN;
        float gsKt    = NAN;
        char  rwy[4]  = "";
    };
    static constexpr int kMax = 7;

    // The aircraft on final from the tracker, dead-reckoned to now; returns
    // how many (up to kMax), nearest first.
    int collect(const TrafficTracker &traffic, unsigned long now, Plane *out);

    // shownMs: how long the screen has been up (the gaps fade in).
    void render(FrameCanvas &c, Plane *planes, int n, uint32_t shownMs, const Sky::Look &sky);

    // A busy westerly morning, for the web page's demo and the docs.
    int demo(Plane *out);
}
