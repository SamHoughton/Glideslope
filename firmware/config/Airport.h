#pragma once
/*
Purpose: The airport the board watches. Heathrow is built in; any other
airport comes from an airport pack (/airport.pack on LittleFS), made by
tools/airport_pack.py and uploaded from the web page.

Pack format: one line of JSON, a newline, then the 128x64 base map as 4096
bytes of 4-bit palette indices (row-major, high nibble = left pixel):

  {"v":1,"icao":"EGKK","iata":"LGW","name":"GATWICK","lat":51.148,"lon":-0.190,
   "tz":"GMT0BST,M3.5.0/1,M10.5.0","skyline":"generic","alternation":false,
   "runways":[{"n":"26L","lat":51.15,"lon":-0.17,"crs":257.8}, ...],
   "map":{"n":51.25,"s":51.05,"w":-0.51,"e":0.13},
   "holds":[{"n":"WILLO","lat":50.985,"lon":-0.191}, ...]}      (holds optional)

Map palette (fixed): 0 empty, 1 river, 2 lakes/reservoirs, 3 runways,
4 aprons, 5 motorways, 6 parks, 7 approach lanes.
*/
#include <Arduino.h>
#include "models/AirportInfo.h"

struct RunwayEnd
{
    char   name[4];     // "27R"
    double lat, lon;    // landing threshold
    float  course;      // landing direction, degrees true
};

struct HoldFix
{
    char   name[6];     // "BNN"
    double lat, lon;
};

struct Airport
{
    static constexpr int kMaxRunways = 16;
    static constexpr int kMaxHolds = 6;

    char      icao[5];
    char      iata[4];
    char      name[17];        // short, upper case: "HEATHROW", "SAN FRANCISCO"
    double    lat, lon;        // reference point
    char      tz[48];          // POSIX TZ rule for local time
    RunwayEnd runways[kMaxRunways];
    uint8_t   runwayCount;
    HoldFix   holds[kMaxHolds];   // named holding stacks (optional)
    uint8_t   holdCount;
    bool      lhrAlternation;  // Heathrow's westerly 15:00 landing-runway swap
    bool      londonSkyline;   // London skyline in the scenes, else a generic one
    double    mapN, mapS, mapW, mapE;
    bool      builtIn;         // true: Heathrow from flash
};

extern Airport g_airport;

namespace AirportPack
{
    constexpr const char *kPath = "/airport.pack";
    constexpr size_t kMapBytes = 128 * 64 / 2;

    // Heathrow, then the pack on LittleFS if there is one (LittleFS mounted).
    void load();
    // Parse a pack's JSON header into out; false with a reason on error.
    bool parseHeader(const char *json, Airport &out, String &error);
    // Base-map palette index at (x, y), and the colour of a palette index.
    uint8_t mapIndex(int x, int y);
    const uint8_t *mapColour(uint8_t index);
    // Whether a route endpoint is this airport.
    bool isHome(const AirportInfo &a);
}
