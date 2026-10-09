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
#include "display/HoldTracker.h"
#include "display/Sky.h"
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

    // One row of the departures page.
    struct Departure
    {
        char   ident[10] = "";
        char   type[6]   = "";
        char   dest[5]   = "";     // IATA (or ICAO) code if the route is known
        char   runway[4] = "";
        Rgb    accent{90, 170, 255};
        time_t at = 0;             // when it climbed out (Unix time; 0 if the clock is unsynced)
    };
    static constexpr int kMaxDepartures = 4;
    void renderDepartures(FrameCanvas &c, const Departure *rows, int n, const char *runways, uint32_t shownMs);

    // Up to four arrivals soonest first, plus the weather line at the bottom.
    // shownMs: how long the board has been up (its letters flip into place).
    void renderArrivals(FrameCanvas &c, const Arrival *rows, int n, const char *runway,
                        const char *weather, unsigned long nowMs, uint32_t shownMs = 100000);
    // Heathrow weather: wind with a compass arrow, visibility, weather,
    // temperature, pressure, and the wind across / along the runway in use.
    // The weather as a scene (sky in the real light, windsock in the real
    // wind, rain or snow falling, temperature) and the numbers beside it.
    void renderWeather(FrameCanvas &c, const Metar &m, const char *runway, unsigned long nowMs,
                       const Sky::Look &sky);

    // Emergency squawk: flashing red frame, code, meaning, call sign, detail.
    void renderAlert(FrameCanvas &c, const char *code, const char *meaning, const char *ident,
                     const char *detail, uint32_t tMs);

    // "WEST 27L ARR" style summary of the runway in use; "" if unknown.
    // Adds the 15:00 runway swap hint for westerly ops before 15:00.
    void runwaySummary(const char *runway, char *out, size_t len, bool withSwap);

    // shownMs: how long the screen has been up (the counters count up and the bars grow).
    void renderStats(FrameCanvas &c, const DailyStats &stats, uint32_t animMs, uint32_t shownMs = 100000);
    // weather: optional dim line under the date ("" for none).
    void renderClock(FrameCanvas &c, uint32_t animMs, const char *weather = "");
    void renderRareBanner(FrameCanvas &c, const char *line1, const char *line2, uint32_t tMs);
    void renderCaption(FrameCanvas &c, const char *text);
    // Holding stacks: one row per stack, its aircraft circling a racetrack.
    void renderHolding(FrameCanvas &c, const HoldTracker::Row *rows, int n, uint32_t animMs);
    // The landing direction has changed (from may be "").
    void renderRunwayChange(FrameCanvas &c, const char *from, const char *to, uint32_t tMs);
}
