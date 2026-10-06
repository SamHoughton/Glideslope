#pragma once
/*
Purpose: The new-contact transition. The aircraft sprite flies across the
panel in the direction the real aircraft is travelling (as seen from the
window the screen faces), climbing or descending with its vertical rate.
Ahead of it the old frame stays put; behind it the new card is revealed with
a short white flash.
*/
#include <Arduino.h>
#include "display/FrameCanvas.h"
#include "display/AircraftSprites.h"

namespace FlyAcross
{
    constexpr uint32_t DURATION_MS = 1700;

    struct Path
    {
        bool rightward = false;   // false: right-to-left (sprite's natural nose-left)
        int  startY    = 4;       // sprite top at entry
        int  endY      = 32;      // sprite top at exit
    };

    // headingDeg: true track; verticalRateFpm: + climbing / - descending
    // (NAN for either is treated as unknown). screenFacing: compass point the
    // viewer looks towards through the screen, e.g. "N", "WSW".
    Path pathFor(double headingDeg, double verticalRateFpm, const char *screenFacing);

    // out must already hold the new card. oldFrame is a snapshot of the panel
    // when the transition began. tMs runs 0..DURATION_MS.
    void compose(FrameCanvas &out, const FrameCanvas &oldFrame, uint32_t tMs,
                 const AircraftSprites::Sprite &sprite, Rgb tail, bool greySprite,
                 const Path &path);
}
