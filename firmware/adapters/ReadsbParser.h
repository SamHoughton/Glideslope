#pragma once
/*
Purpose: Parse readsb-style aircraft JSON (tar1090's aircraft.json, and the
adsb.lol / adsb.fi community APIs) into StateVectors: stale and position-less
entries dropped, radius and minimum-altitude filters applied, units
converted to the StateVector SI convention (m, m/s, m/s).
*/
#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>
#include "models/StateVector.h"

namespace ReadsbParser
{
    // Filter document for deserializeJson: keeps only the fields we use.
    // arrayKey is "aircraft" (tar1090) or "ac" (community APIs).
    void buildFilter(JsonDocument &filter, const char *arrayKey);

    // Returns the number of aircraft in the array (before filtering).
    int parse(JsonArray aircraft, double centerLat, double centerLon, double radiusKm,
              std::vector<StateVector> &out);
}
