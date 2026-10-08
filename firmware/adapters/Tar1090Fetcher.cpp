/*
Purpose: Fetch ADS-B state vectors from a local tar1090 receiver over HTTP.
Responsibilities:
- GET http://<g_config.tar1090_host>/data/aircraft.json
- Parse with ReadsbParser (stale/position-less entries dropped, radius and
  altitude filters, units converted to the StateVector SI convention).
Returns false (signals fallback) if host is unconfigured or the request fails.
*/
#include "adapters/Tar1090Fetcher.h"
#include "adapters/ReadsbParser.h"
#include "config/RuntimeConfig.h"
#include "config/TimingConfiguration.h"
#include "utils/TelnetLogger.h"

bool Tar1090Fetcher::fetchStateVectors(double centerLat,
                                        double centerLon,
                                        double radiusKm,
                                        std::vector<StateVector> &outStateVectors)
{
    if (strlen(g_config.tar1090_host) == 0)
        return false;   // not configured — caller falls back

    String url = String("http://") + g_config.tar1090_host + "/data/aircraft.json";

    HTTPClient http;
    http.begin(url);
    http.setTimeout(TimingConfiguration::LOCAL_ADSB_TIMEOUT_MS);

    int code = http.GET();
    if (code != 200)
    {
        Log.printf("Tar1090Fetcher: HTTP %d from %s\n", code, url.c_str());
        http.end();
        return false;
    }

    JsonDocument filter;
    ReadsbParser::buildFilter(filter, "aircraft");
    JsonDocument doc;
    DeserializationError err = deserializeJson(
        doc, http.getStream(), DeserializationOption::Filter(filter));
    http.end();

    if (err)
    {
        Log.printf("Tar1090Fetcher: JSON parse error: %s\n", err.c_str());
        return false;
    }

    JsonArray aircraft = doc["aircraft"].as<JsonArray>();
    if (aircraft.isNull())
    {
        Log.println("Tar1090Fetcher: no aircraft array in response");
        return false;
    }

    std::vector<StateVector> fresh;
    const int tracked = ReadsbParser::parse(aircraft, centerLat, centerLon, radiusKm, fresh);
    outStateVectors.swap(fresh);
    Log.printf("Tar1090Fetcher: %d tracked, %u within %.0f km\n",
               tracked, (unsigned)outStateVectors.size(), radiusKm);
    return true;
}
