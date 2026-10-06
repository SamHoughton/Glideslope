#pragma once
/*
Purpose: Boot / idle screen. "SCANNING" with animated dots, a dotted horizon
under twinkling stars, and approach lights chasing towards the runway
threshold, with the airport (and the runway in use, once known) below.
Shown from power-up until the first card flies in, and whenever nothing is in range.
*/
#include <Arduino.h>
#include "display/FrameCanvas.h"

namespace BootSplash
{
    // runway: e.g. "27L"; empty when not yet known.
    void render(FrameCanvas &c, uint32_t tMs, const char *runway);
}
