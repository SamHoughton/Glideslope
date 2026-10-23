#pragma once
/*
Purpose: The logbook (8.1): what the board has seen, kept in flash for good.

  /log/days.bin        one Logbook::Day a day (64 bytes; a year is 23 KB)
  /log/events.bin      go-arounds, emergencies, rare types, first visits,
                       watchlist hits (24 bytes each)
  /log/collection.bin  every airline and type ever seen: first and last
                       day, days seen, how many (20 bytes each)
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

    // CSV for the web page, written straight to the client.
    void writeDaysCsv(WiFiClient &c);
    void writeEventsCsv(WiFiClient &c);
    void writeCollectionCsv(WiFiClient &c);
    void recordsJson(String &out);        // small
    size_t bytesUsed();
}
