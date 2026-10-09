#pragma once
/*
Purpose: Touchdown animation, played once when the aircraft on the card
reaches the runway threshold. Side view against a London skyline: the plane
descends over the approach lights, flares, touches down past the piano keys
with tyre smoke, rolls out and slows to a stop; "LANDED 27L" and the callsign
appear above.
GoAround instead comes down towards the threshold, pitches up and climbs
away under a flashing "GO AROUND". Takeoff starts at the threshold, rolls,
rotates and climbs out over the skyline under "DEPARTED 27R".
Drawn for right-to-left travel and mirrored for left-to-right.
The sky (display/Sky.h) sets the light: a day, golden-hour, twilight or night
backdrop with the weather, lit windows and runway lights after dark, and the
aircraft's landing lights, beacon and strobes.
*/
#include <Arduino.h>
#include "display/FrameCanvas.h"
#include "display/AircraftSprites.h"
#include "display/Sky.h"

namespace LandingScene
{
    constexpr uint32_t DURATION_MS = 3400;

    enum Kind : uint8_t { Landing, GoAround, Takeoff };

    // caption2: the second caption line (call sign, or "BA117 TO JFK").
    void render(FrameCanvas &c, uint32_t tMs, const AircraftSprites::Sprite &sprite,
                Rgb accent, bool greySprite, bool rightward,
                const char *caption2, const char *runway, Kind kind = Landing,
                const Sky::Look &sky = Sky::Look(), const AircraftSprites::Livery *livery = nullptr);
}
