#pragma once
/*
Purpose: An aircraft passing over the home marker (or one on the
watchlist): it crosses the sky at a size set by its height, low ones big,
high ones small with contrails above 25,000 ft, under the real sky. A label
(OVERHEAD, WATCHLIST) top left, the altitude top right, and the flight,
where it's going and its type along the bottom.
*/
#include <Arduino.h>
#include "display/FrameCanvas.h"
#include "display/AircraftSprites.h"
#include "display/Sky.h"

namespace OverheadScene
{
constexpr uint32_t DURATION_MS = 7000;

struct Info
{
    char  label[12] = "OVERHEAD";
    Rgb   labelColour{255, 185, 60};
    char  ident[10] = "";
    char  type[12] = "";          // display name or ICAO code
    char  where[16] = "";         // "TO DUBAI", "FROM ROME" or ""
    float altFt = NAN;
    Rgb   accent{90, 170, 255};
    AircraftSprites::Kind kind = AircraftSprites::A320;
    bool  known = true;           // false: grey fallback sprite
    AircraftSprites::Livery livery;
    bool  rightward = true;       // direction across the panel
};

void render(FrameCanvas &c, const Info &info, uint32_t tMs, const Sky::Look &sky);
}
