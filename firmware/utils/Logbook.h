#pragma once
/*
Purpose: The logbook (8.1): what the board has seen, kept in flash for good.

  /log/days.bin        one Logbook::Day a day (64 bytes; a year is 23 KB)
  /log/events.bin      go-arounds, emergencies, rare types, first visits,
                       watchlist hits (24 bytes each)
  /log/collection.bin  every airline and type ever seen, and (9.0) every
                       airport arrivals came from: first and last day, days
                       seen, how many (20 bytes each)
  /log/airframes.bin   every airframe (utils/Airframes.h)
  /log/records.bin     the bests: busiest day, most go-arounds, tightest gap

Written from the main loop (never the display task): a day at midnight, an
event as it happens. Read back by the web page as CSV, streamed a line at a
time, so nothing big is ever held in RAM.
*/
#include <Arduino.h>
#include <vector>
#include "display/LogbookTypes.h"

class WiFiClient;

namespace Logbook
{
    void begin();                         // LittleFS mounted
    const Records &records();

    // The finished day and its airlines and types. Updates the records and
    // the collection; news (a new record, a collection milestone) comes
    // back in line1/line2 (empty if none).
    void addDay(const Day &d, const std::vector<Tally> &tallies, char *line1, size_t len1, char *line2, size_t len2);
    void addEvent(const Event &e);
    // An arrival from this airport (ICAO), as it is seen (9.0): into the
    // collection at once, each flight counted once. True the first time
    // the airport is ever seen (once the collection is a fortnight old:
    // before that nearly everything is new).
    bool noteOrigin(const char *icao, const char *ident);

    // CSV for the web page, written straight to the client.
    void writeDaysCsv(WiFiClient &c);
    void writeEventsCsv(WiFiClient &c);
    void writeCollectionCsv(WiFiClient &c);
    void writeAirframesCsv(WiFiClient &c);
    void recordsJson(String &out);        // small
    // The year's summary (9.0): a pass over each file (a few hundred ms);
    // false if nothing was logged that year.
    bool year(uint16_t year, Year &out);
    size_t bytesUsed();
}
