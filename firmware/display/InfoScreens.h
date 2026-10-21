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
#include "models/FlightInfo.h"
#include "utils/Weather.h"

namespace InfoScreens
{
    // One row of the arrivals board.
    struct Arrival
    {
        char  ident[10] = "";
        char  type[6]   = "";
        char  from[5]   = "";     // origin IATA (or ICAO) code if the route is known
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
    // Where the recent departures are heading (8.0): a compass round the
    // airport, a ray along each one's course to its destination (newest
    // brightest), the flights and destinations beside it. How many have a
    // destination it can place; under two, nothing is drawn.
    int departureHeadings(const Departure *rows, int n);
    void renderHeadings(FrameCanvas &c, const Departure *rows, int n, uint32_t shownMs);

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
    // On final, between the card's appearances: the aircraft itself (sprite
    // in its livery, type, registration, how often it has been here), with
    // the countdown kept top right. etaSec < 0: no countdown.
    void renderAircraft(FrameCanvas &c, const FlightInfo &f, Rgb accent, float etaSec, const char *runway,
                        uint32_t shownMs);
    // On final (8.0): where it came from. The sky there now (its own sun,
    // its weather) with the temperature, its local time and weather in
    // words, roughly how long it has been flying and how far. wx: the
    // origin's weather if known (else nullptr). False if the origin is
    // unknown or too close to be worth a page.
    bool renderOrigin(FrameCanvas &c, const FlightInfo &f, const Metar *wx, float etaSec, const char *runway,
                      uint32_t shownMs);
    // Second and third stats pages: the busiest airlines and aircraft types.
    void renderTopAirlines(FrameCanvas &c, const DailyStats &stats, uint32_t shownMs);
    void renderTopTypes(FrameCanvas &c, const DailyStats &stats, uint32_t shownMs);
    // 7.1 stats pages: how often they are landing (the recent gap, today's
    // average and tightest, each runway's share), and today's spotter log.
    void renderRhythm(FrameCanvas &c, const DailyStats &stats, const char *runway, uint32_t shownMs);
    void renderSpots(FrameCanvas &c, const DailyStats &stats, uint32_t shownMs);
    // Overnight, in turn with the clock: the day in one card.
    void renderDaySummary(FrameCanvas &c, const DailyStats &stats, uint32_t animMs);
    // weather: optional dim line under the date ("" for none).
    void renderClock(FrameCanvas &c, uint32_t animMs, const char *weather = "");
    // The night screen (7.2): the skyline under the real sky, a row of
    // distant runway lights, the time above, the date taking turns with the
    // next sunrise and the weather, and now and then an aircraft's lights
    // crossing far off. Falls back to the plain clock without a clock.
    void renderNightScene(FrameCanvas &c, uint32_t animMs, const char *weather, const Sky::Look &sky,
                          bool decorativePlane = true);
    void renderRareBanner(FrameCanvas &c, const char *line1, const char *line2, uint32_t tMs);
    void renderCaption(FrameCanvas &c, const char *text);
    // Holding stacks: one row per stack, its aircraft circling a racetrack.
    void renderHolding(FrameCanvas &c, const HoldTracker::Row *rows, int n, uint32_t animMs);
    // The landing direction has changed (from may be "").
    void renderRunwayChange(FrameCanvas &c, const char *from, const char *to, uint32_t tMs);
}
