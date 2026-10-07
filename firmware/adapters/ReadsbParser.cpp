#include "adapters/ReadsbParser.h"
#include "config/RuntimeConfig.h"
#include "utils/GeoUtils.h"

void ReadsbParser::buildFilter(JsonDocument &filter, const char *arrayKey)
{
    JsonObject f = filter[arrayKey][0].to<JsonObject>();
    for (const char *k : {"hex", "flight", "r", "t", "lat", "lon", "alt_baro", "gs",
                          "track", "baro_rate", "seen_pos", "squawk"})
        f[k] = true;
}

int ReadsbParser::parse(JsonArray aircraft, double centerLat, double centerLon, double radiusKm,
                        std::vector<StateVector> &out)
{
    int tracked = 0;
    for (JsonObject a : aircraft)
    {
        ++tracked;

        // Stale position (> 30 s since the last fix), or none at all.
        if ((a["seen_pos"] | 999.0f) > 30.0f) continue;
        if (a["lat"].isNull() || a["lon"].isNull()) continue;

        const double lat = a["lat"].as<double>();
        const double lon = a["lon"].as<double>();
        const double dist = haversineKm(centerLat, centerLon, lat, lon);
        if (dist > radiusKm) continue;

        // readsb reports aircraft on the ground as alt_baro "ground" (a string).
        const bool onGround = a["alt_baro"].is<const char *>();
        if (onGround) continue;
        if (g_config.min_altitude_ft >= 0)
        {
            const float altFt = a["alt_baro"] | -1.0f;
            if (altFt >= 0 && altFt < (float)g_config.min_altitude_ft) continue;
        }

        StateVector s;
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
        if (!a["baro_rate"].isNull()) s.vertical_rate = a["baro_rate"].as<double>() / 196.85; // fpm -> m/s

        s.distance_km = dist;
        s.bearing_deg = computeBearingDeg(centerLat, centerLon, lat, lon);
        out.push_back(s);
    }
    return tracked;
}
