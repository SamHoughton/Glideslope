#pragma once
/*
Purpose: Heathrow's latest METAR (aviationweather.gov, free, no key) and a
short line for the panel: wind, gusts, visibility, weather and temperature,
e.g. "310/04G15 7KM RA 14C". Fetched every few minutes on the main loop.

  GET https://aviationweather.gov/api/data/metar?ids=EGLL&format=raw
  -> "METAR EGLL 070850Z 31004KT 7000 RA FEW025 14/12 Q1012"
*/
#include <Arduino.h>

struct Metar
{
    bool  valid   = false;
    int   windDir = -1;       // degrees true; -1 = variable / calm
    int   windKt  = 0;
    int   gustKt  = 0;        // 0 = no gusts
    int   visM    = -1;       // metres; 9999 = 10 km or more; -1 = unknown
    bool  cavok   = false;
    int   tempC   = -99;      // -99 = unknown
    int   qnh     = 0;        // hPa; 0 = unknown
    int   altInHg100 = 0;     // US altimeter setting (A2989 -> 2989); 0 = given in hPa
    char  visText[8] = "";    // visibility as the panel shows it: "10KM+", "7KM", "800M", "10SM", "1.5SM"
    char  wx[8]   = "";       // present weather, e.g. "RA", "+TSRA" (first group only)
    char  raw[112] = "";
};

namespace Weather
{
    // Fetch and parse EGLL's METAR; false on a network or parse failure.
    bool fetch(Metar &out);
    bool parse(const char *raw, Metar &out);
    // Panel line, at most maxChars characters (drops detail to fit).
    void line(const Metar &m, char *out, size_t len, int maxChars = 21);
    // Pressure as the panel shows it: "Q1012", or "A29.89" where the report uses inches.
    void pressure(const Metar &m, char *out, size_t len);
    // Wind component across / along a runway heading (kt; tail > 0 = tailwind).
    void components(const Metar &m, int runwayDeg, int &crossKt, int &tailKt);
}
