#pragma once
/*
Purpose: Decide whether an aircraft deserves the rare-spot flourish: an A380,
747, An-124/225, military traffic, or a type never seen before. Seen types
are logged to LittleFS (/types.txt, one ICAO type per line) so "first
sighting" survives reboots.
*/
#include <Arduino.h>
#include "models/FlightInfo.h"

namespace RareSpotter
{
    enum Reason : uint8_t { None, Rare, Military, FirstSeen };

    // Load the type log (call once after LittleFS is mounted).
    void begin();

    // Classify, and record the type as seen. Call once per aircraft shown.
    Reason check(const FlightInfo &f);

    // Banner lines for a reason, e.g. "RARE SPOT" / "A380".
    void banner(Reason r, const FlightInfo &f, char *line1, size_t len1, char *line2, size_t len2);
}
