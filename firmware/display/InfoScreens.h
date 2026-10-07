#pragma once
/*
Purpose: The smaller full-panel screens: daily stats, the quiet-hours clock,
the rare-spot banner, a short mode-change caption, and the runway-in-use
summary shared by the map and the scanning screen.
*/
#include <Arduino.h>
#include "display/FrameCanvas.h"
#include "display/DailyStats.h"

namespace InfoScreens
{
    // "WEST 27L ARR" style summary of the runway in use; "" if unknown.
    // Adds the 15:00 runway swap hint for westerly ops before 15:00.
    void runwaySummary(const char *runway, char *out, size_t len, bool withSwap);

    void renderStats(FrameCanvas &c, const DailyStats &stats, uint32_t animMs);
    void renderClock(FrameCanvas &c, uint32_t animMs);
    void renderRareBanner(FrameCanvas &c, const char *line1, const char *line2, uint32_t tMs);
    void renderCaption(FrameCanvas &c, const char *text);
}
