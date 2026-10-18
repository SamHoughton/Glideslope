#include "adapters/ReadsbParser.h"
#include "config/RuntimeConfig.h"
#include "utils/GeoUtils.h"

void ReadsbParser::buildFilter(JsonDocument &filter, const char *arrayKey)
{
    JsonObject f = filter[arrayKey][0].to<JsonObject>();
    for (const char *k : {"hex", "flight", "r", "t", "lat", "lon", "alt_baro", "gs",
                          "track", "baro_rate", "geom_rate", "seen_pos", "squawk"})
        f[k] = true;
}

namespace
{
    // One aircraft: false if it is stale, outside the radius, on the ground
    // or below the minimum altitude.
    bool one(JsonObject a, double centerLat, double centerLon, double radiusKm, StateVector &s)
    {
        // Stale position (> 30 s since the last fix), or none at all.
        if ((a["seen_pos"] | 999.0f) > 30.0f) return false;
        if (a["lat"].isNull() || a["lon"].isNull()) return false;

        const double lat = a["lat"].as<double>();
        const double lon = a["lon"].as<double>();
        const double dist = haversineKm(centerLat, centerLon, lat, lon);
        if (dist > radiusKm) return false;

        // readsb reports aircraft on the ground as alt_baro "ground" (a string).
        if (a["alt_baro"].is<const char *>()) return false;
        if (g_config.min_altitude_ft >= 0)
        {
            const float altFt = a["alt_baro"] | -1.0f;
            if (altFt >= 0 && altFt < (float)g_config.min_altitude_ft) return false;
        }

        s.icao24 = a["hex"] | "";
        String cs = a["flight"] | "";
        cs.trim();
        s.callsign = cs;
        String reg = a["r"] | "";
        reg.trim();
        s.registration = reg;
        String typ = a["t"] | "";
        typ.trim();
        s.aircraft_type = typ;
        s.squawk = a["squawk"] | "";
        s.lat = lat;
        s.lon = lon;

        // readsb units -> StateVector SI (FlightDataFetcher converts back).
        if (!a["alt_baro"].isNull()) s.baro_altitude = a["alt_baro"].as<double>() / 3.28084;  // ft -> m
        if (!a["gs"].isNull())       s.velocity      = a["gs"].as<double>() / 1.94384;        // kt -> m/s
        if (!a["track"].isNull())    s.heading       = a["track"].as<double>();
        // Climb rate: barometric, else geometric (GNSS); low down adsb.lol
        // often has only the second, and without it a departure looks level.
        if (!a["baro_rate"].isNull())      s.vertical_rate = a["baro_rate"].as<double>() / 196.85; // fpm -> m/s
        else if (!a["geom_rate"].isNull()) s.vertical_rate = a["geom_rate"].as<double>() / 196.85;

        s.distance_km = dist;
        s.bearing_deg = computeBearingDeg(centerLat, centerLon, lat, lon);
        return true;
    }
}

int ReadsbParser::parse(JsonArray aircraft, double centerLat, double centerLon, double radiusKm,
                        std::vector<StateVector> &out)
{
    int tracked = 0;
    for (JsonObject a : aircraft)
    {
        ++tracked;
        StateVector s;
        if (one(a, centerLat, centerLon, radiusKm, s)) out.push_back(s);
    }
    return tracked;
}

int ReadsbParser::parseStream(Stream &in, const char *arrayKey, double centerLat, double centerLon,
                              double radiusKm, std::vector<StateVector> &out)
{
    char key[16];
    snprintf(key, sizeof(key), "\"%s\":[", arrayKey);
    if (!in.find(key)) return -1;

    JsonDocument filter;
    for (const char *k : {"hex", "flight", "r", "t", "lat", "lon", "alt_baro", "gs",
                          "track", "baro_rate", "geom_rate", "seen_pos", "squawk"})
        filter[k] = true;
    // peek() does not wait (on a TLS stream the next record may still be on
    // its way), so wait for a byte first, up to the stream's timeout.
    auto next = [&in]() -> int {
        const unsigned long t0 = millis();
        while (!in.available() && millis() - t0 < 4000) delay(1);
        return in.peek();
    };
    JsonDocument doc;
    const size_t reserved = out.capacity();
    int tracked = 0;
    for (;;)
    {
        // Skip separators and stop at the end of the array.
        int ch;
        while ((ch = next()) == ' ' || ch == '\n' || ch == '\r' || ch == '\t' || ch == ',') in.read();
        if (ch == ']' || ch < 0)
        {
            if (ch == ']') in.read();
            break;
        }
        if (deserializeJson(doc, in, DeserializationOption::Filter(filter))) break;
        ++tracked;
        StateVector s;
        // Never past a capacity the caller reserved beforehand (a
        // reallocation needs a second, larger block while the old one is
        // still held). With none reserved the list grows as usual.
        if (one(doc.as<JsonObject>(), centerLat, centerLon, radiusKm, s) && (!reserved || out.size() < reserved))
            out.push_back(s);
    }
    return tracked;
}
