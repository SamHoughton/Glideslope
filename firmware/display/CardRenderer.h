#pragma once
/*
Purpose: Draw one flight card into a FrameCanvas.

Layout (128x64), inside an optional 1px border in the airline accent:
  left column x2-45        right column x48-125
  y 2-33  airline logo     y 2   callsign (2x font)
                           y 18  route
  y 35-51 aircraft sprite  y 27  aircraft type
                           y 36  altitude + speed
                           y 45  status, e.g. APPROACH 27L (pulsing)
  y 55-61 ETA / distance  |  threshold bars + progress strip with marker
*/
#include <Arduino.h>
#include "display/FrameCanvas.h"
#include "display/ApproachModel.h"
#include "models/FlightInfo.h"

namespace CardRenderer
{
    // Accent colour for a flight: the dominant saturated colour of its logo,
    // or a default blue when there is no logo.
    Rgb accentFor(const FlightInfo &f);

    struct Options
    {
        uint32_t animMs       = 0;      // free-running clock: pulses, glint, approach lights
        uint32_t dataAgeMs    = 0;      // time since telemetry arrived (altitude dead-reckoning)
        bool     border       = true;   // 1px panel border in the accent colour
        bool     spriteRight  = false;  // sprite faces right (aircraft moving rightward)
        bool     landed       = false;  // after the landing animation
        time_t   landedAt     = 0;      // touchdown time (Unix), 0 if the clock wasn't set
        double   altFt        = NAN;    // smoothed altitude to show; NAN = dead-reckon here
    };

    void render(FrameCanvas &c, const FlightInfo &f, const ApproachStatus &s,
                Rgb accent, const Options &opt);
}
