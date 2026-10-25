#pragma once
/*
Purpose: The logbook's records (8.1), shared by the day's tally (which
makes them) and utils/Logbook (which keeps them in flash). Fixed sizes, so
the files are arrays of them.
*/
#include <stdint.h>

namespace Logbook
{
    // One day, written after midnight to /log/days.bin. 64 bytes.
    struct Day
    {
        uint32_t date = 0;               // YYYYMMDD, local
        uint16_t arrivals = 0, departures = 0;
        uint8_t  goArounds = 0, types = 0, airlines = 0, spare0 = 0;
        uint8_t  hours[24] = {};         // movements in each local hour (255 at most)
        char     busiest[4] = "";        // busiest airline (IATA, else ICAO)
        uint16_t busiestN = 0;
        uint16_t tightestSec = 0, avgGapSec = 0;
        char     runway[4] = "";         // the runway most arrivals used
        uint16_t runwayN = 0;
        uint8_t  reserved[12] = {};
    };
    static_assert(sizeof(Day) == 64, "Logbook::Day is 64 bytes on disk");

    // Something worth remembering, appended to /log/events.bin. 24 bytes.
    struct Event
    {
        uint32_t at = 0;                 // Unix time
        uint8_t  kind = 0;               // DailyStats::SpotKind
        char     ident[8] = "";
        char     what[9] = "";
        uint8_t  spare[2] = {};
    };
    static_assert(sizeof(Event) == 24, "Logbook::Event is 24 bytes on disk");

    // A day's count of one airline or type, handed over at midnight.
    struct Tally
    {
        char     code[6];
        uint8_t  kind;                   // 0 airline, 1 type
        uint16_t n;
    };

    // A year at the airport (9.0), worked out from the logbook for the
    // panel's year screen and the web page. Not kept on disk.
    struct Year
    {
        uint16_t year = 0, days = 0;          // days logged
        uint32_t arrivals = 0, departures = 0;
        uint32_t bestDayDate = 0;             // YYYYMMDD
        uint16_t bestDay = 0;
        uint16_t goArounds = 0, rares = 0;
        uint16_t tightest = 0;                // seconds (0: none)
        uint32_t tightDate = 0;
        uint8_t  busiestHour = 0;             // local hour with the most movements over the year
        uint16_t newAirlines = 0, newTypes = 0, newAirports = 0, newAirframes = 0;   // first seen this year
        char     topReg[8] = "";              // the airframe seen on the most days
        char     topType[5] = "";
        uint16_t topDays = 0;
    };

    // The all-time bests, /log/records.bin.
    struct Records
    {
        uint32_t bestDayDate = 0;        // YYYYMMDD
        uint16_t bestDay = 0;            // movements
        uint8_t  bestGa = 0, spare0 = 0;
        uint32_t bestGaDate = 0;
        uint16_t tightest = 0;           // seconds (0: none yet)
        uint16_t airports = 0;           // origin airports in the collection (9.0; a spare before)
        uint32_t tightDate = 0;
        uint16_t airlines = 0, types = 0;    // in the collection
        uint32_t since = 0;              // YYYYMMDD of the first day logged
    };
}
