#pragma once
/*
Purpose: Heathrow's latest METAR (aviationweather.gov, free, no key) and a
short line for the panel: wind, gusts, visibility, weather and temperature,
e.g. "310/04G15 7KM RA 14C". Fetched every few minutes on the main loop.

  GET https://aviationweather.gov/api/data/metar?ids=EGLL&format=raw&taf=true
  -> "METAR EGLL 070850Z 31004KT 7000 RA FEW025 14/12 Q1012"
     "TAF EGLL 070500Z 0706/0812 30008KT 9999 SCT030"
     "  BECMG 0714/0717 06012KT" ...

8.2: the TAF that comes with it (the airport's own; the origins' are
skipped), read line by line as it arrives, for the runway outlook; and the
airfield's mood from the METAR (low visibility, thunderstorms).
*/
#include <Arduino.h>
#include <time.h>

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
    int16_t ceilingFt = -1;   // lowest broken/overcast layer or vertical visibility; -1 = none
    bool  thunder = false;    // TS in any weather group at the airport
    bool  thunderNear = false;   // VCTS: in the vicinity
    bool  cb      = false;    // cumulonimbus or towering cumulus reported
    char  raw[112] = "";
    char  station[5] = "";    // ICAO, e.g. "EGLL"
};

// The forecast wind through a TAF's validity: the prevailing wind from each
// change on (the base forecast, FM and BECMG groups; TEMPO and PROB groups
// are passing and left out).
struct Taf
{
    struct Wind { time_t at; int16_t dir; uint8_t kt, gust; };   // dir -1 = variable
    bool    valid = false;
    time_t  issued = 0, from = 0, to = 0;
    Wind    w[12];
    uint8_t n = 0;
    uint8_t mode = 0;     // the parser's place (between lines)
    time_t  pending = 0;  // the time the current group's wind takes effect
    bool    groupWind = false;
};

namespace Weather
{
    // The airfield's mood from a METAR (8.2). Low visibility: under 1,000 m
    // or a ceiling at 200 ft or below, until better than 1,500 m and 300 ft.
    enum Mood : uint8_t { Calm = 0, LowVis = 1, Storm = 2, StormNear = 3 };
    Mood mood(const Metar &m, Mood previous = Calm);
    // For the banner and the phone: "Low visibility" / "Fog, 300 m".
    void moodLines(Mood k, const Metar &m, char *l1, size_t l1Len, char *l2, size_t l2Len);

    // TAF: feed it text (whole, or a line at a time as it arrives); times are
    // placed in the month around now (UTC).
    void tafBegin(Taf &t);
    void tafFeed(Taf &t, const char *text, time_t now);
    // The forecast prevailing wind at time t; false outside the TAF.
    bool tafWindAt(const Taf &t, time_t when, Taf::Wind &out);

    // When the landing direction should change, from the TAF: the first
    // forecast wind that puts 5 kt or more of tailwind on the runways in use
    // (or, at Heathrow on easterlies, lets them go back to the westerly
    // preference). currentCourse: the landing course in use (degrees true).
    struct Outlook
    {
        bool   change = false;
        time_t at = 0;            // when (now if it is already due)
        char   to[4] = "";        // "09"
        int16_t windDir = -1;
        uint8_t windKt = 0;
    };
    bool runwayOutlook(const Taf &t, float currentCourse, time_t now, Outlook &out);

    // Fetch and parse the airport's METAR; false on a network or parse
    // failure. others: more ICAO codes for the same request ("OMDB,KJFK"),
    // each report handed to onOther (8.0: the weather where arrivals came from).
    // taf: the airport's TAF too (from the same reply), nullptr to skip it.
    bool fetch(Metar &out, const char *others = nullptr, void (*onOther)(const Metar &) = nullptr, Taf *taf = nullptr);
    bool parse(const char *raw, Metar &out);
    // Panel line, at most maxChars characters (drops detail to fit).
    void line(const Metar &m, char *out, size_t len, int maxChars = 21);
    // In words for the 7.0 screens, e.g. "11°  light rain  250/14G26" (UTF-8
    // degree sign); the wind is left off when it would pass maxChars.
    void friendly(const Metar &m, char *out, size_t len, int maxChars = 26);
    // The present-weather group in words ("-RA" -> "light rain"); "" if none.
    void describe(const char *wx, char *out, size_t len);
    // Pressure as the panel shows it: "Q1012", or "A29.89" where the report uses inches.
    void pressure(const Metar &m, char *out, size_t len);
    // Wind component across / along a runway heading (kt; tail > 0 = tailwind).
    void components(const Metar &m, int runwayDeg, int &crossKt, int &tailKt);
}
