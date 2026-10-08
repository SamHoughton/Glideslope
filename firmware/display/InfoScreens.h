#pragma once
/*
Purpose: The smaller full-panel screens: daily stats, the next-arrivals
board, the quiet-hours clock, the rare-spot banner, the emergency-squawk
alert, a short mode-change caption, and the runway-in-use summary shared by
the map and the scanning screen.
*/
#include <Arduino.h>
#include "display/FrameCanvas.h"
#include "display/DailyStats.h"
#include "utils/Weather.h"

namespace InfoScreens
{
    // One row of the arrivals board.
    struct Arrival
    {
        char  ident[10] = "";
        char  type[6]   = "";
        Rgb   accent{90, 170, 255};
        float etaSec    = NAN;    // at dataMs
        bool  estimate  = false;  // not yet on final: a rough guess
        unsigned long dataMs = 0;
    };
    static constexpr int kMaxArrivals = 4;

    // Up to four arrivals soonest first, plus the weather line at the bottom.
    void renderArrivals(FrameCanvas &c, const Arrival *rows, int n, const char *runway,
                        const char *weather, unsigned long nowMs);
    // Heathrow weather: wind with a compass arrow, visibility, weather,
    // temperature, pressure, and the wind across / along the runway in use.
    void renderWeather(FrameCanvas &c, const Metar &m, const char *runway, unsigned long nowMs);

    // Emergency squawk: flashing red frame, code, meaning, call sign, detail.
    void renderAlert(FrameCanvas &c, const char *code, const char *meaning, const char *ident,
                     const char *detail, uint32_t tMs);

    // "WEST 27L ARR" style summary of the runway in use; "" if unknown.
    // Adds the 15:00 runway swap hint for westerly ops before 15:00.
    void runwaySummary(const char *runway, char *out, size_t len, bool withSwap);

    void renderStats(FrameCanvas &c, const DailyStats &stats, uint32_t animMs);
    // weather: optional dim line under the date ("" for none).
    void renderClock(FrameCanvas &c, uint32_t animMs, const char *weather = "");
    void renderRareBanner(FrameCanvas &c, const char *line1, const char *line2, uint32_t tMs);
    void renderCaption(FrameCanvas &c, const char *text);
    // The landing direction has changed (from may be "").
    void renderRunwayChange(FrameCanvas &c, const char *from, const char *to, uint32_t tMs);
}
