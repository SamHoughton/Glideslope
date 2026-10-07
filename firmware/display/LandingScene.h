#pragma once
/*
Purpose: Touchdown animation, played once when the aircraft on the card
reaches the runway threshold. Side view against a London skyline: the plane
descends over the approach lights, flares, touches down past the piano keys
with tyre smoke, rolls out and slows to a stop; "LANDED 27L" and the callsign
appear above.
With goAround set it instead comes down towards the threshold, pitches up
and climbs away under a flashing "GO AROUND".
Drawn for right-to-left travel and mirrored for left-to-right.
*/
#include <Arduino.h>
#include "display/FrameCanvas.h"
#include "display/AircraftSprites.h"

namespace LandingScene
{
    constexpr uint32_t DURATION_MS = 3400;

    void render(FrameCanvas &c, uint32_t tMs, const AircraftSprites::Sprite &sprite,
                Rgb accent, bool greySprite, bool rightward,
                const String &ident, const char *runway, bool goAround = false);
}
