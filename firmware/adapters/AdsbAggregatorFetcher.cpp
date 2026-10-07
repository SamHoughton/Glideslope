#include "adapters/AdsbAggregatorFetcher.h"
#include "adapters/ReadsbParser.h"
#include "config/RuntimeConfig.h"
#include "utils/TelnetLogger.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

namespace
{
    constexpr uint32_t kTimeoutMs = 6000;
    constexpr uint32_t kBackoffMs = 60000;   // rest a service for a minute after HTTP 429
}

bool AdsbAggregatorFetcher::fetchFrom(int i, const String &url, const char *name, double centerLat,
                                      double centerLon, double radiusKm, std::vector<StateVector> &out)
{
    if (_restUntil[i] && (long)(millis() - _restUntil[i]) < 0)
        return false;
    _restUntil[i] = 0;

    WiFiClientSecure client;
    client.setInsecure();   // public open data; matches the project's other HTTPS clients
    HTTPClient http;
    if (!http.begin(client, url))
        return false;
    http.setTimeout(kTimeoutMs);
    http.setUserAgent("Glideslope/1.0 (+https://github.com/SamHoughton/Glideslope)");
    http.useHTTP10(true);   // no chunked encoding, so the body streams straight into the parser

    const int code = http.GET();
    if (code != 200)
    {
        if (code == 429)
        {
            _restUntil[i] = millis() + kBackoffMs;
            Log.printf("AdsbAggregatorFetcher: %s rate-limited, resting it for 60 s\n", name);
        }
        else
            Log.printf("AdsbAggregatorFetcher: %s HTTP %d\n", name, code);
        http.end();
        return false;
    }

    // adsb.lol lists aircraft under "ac", adsb.fi under "aircraft": keep both.
    JsonDocument filter;
    ReadsbParser::buildFilter(filter, "ac");
    ReadsbParser::buildFilter(filter, "aircraft");
    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, http.getStream(),
                                                     DeserializationOption::Filter(filter));
    http.end();
    if (err)
    {
        Log.printf("AdsbAggregatorFetcher: %s JSON error: %s\n", name, err.c_str());
        return false;
    }
    JsonArray ac = doc["ac"].as<JsonArray>();
    if (ac.isNull()) ac = doc["aircraft"].as<JsonArray>();
    if (ac.isNull())
    {
        Log.printf("AdsbAggregatorFetcher: %s returned no aircraft list\n", name);
        return false;
    }
    ReadsbParser::parse(ac, centerLat, centerLon, radiusKm, out);
    _lastSource = name;
    return true;
}

bool AdsbAggregatorFetcher::fetchStateVectors(double centerLat, double centerLon, double radiusKm,
                                              std::vector<StateVector> &outStateVectors)
{
    _lastSource = "";
    if (!g_config.use_community_feeds)
        return false;

    // Both APIs take the radius in nautical miles.
    const int nm = max(1, (int)ceil(radiusKm / 1.852));
    char lat[16], lon[16];
    snprintf(lat, sizeof(lat), "%.4f", centerLat);
    snprintf(lon, sizeof(lon), "%.4f", centerLon);
    const String urls[2] = {
        String("https://api.adsb.lol/v2/point/") + lat + "/" + lon + "/" + nm,
        String("https://opendata.adsb.fi/api/v2/lat/") + lat + "/lon/" + lon + "/dist/" + nm,
    };
    static const char *const kNames[2] = { "adsb.lol", "adsb.fi" };

    // Alternate which service goes first, so each is asked half as often (and
    // stays well inside its rate limit); the other is the immediate backup.
    _turn ^= 1;
    for (int k = 0; k < 2; ++k)
    {
        const int i = (_turn + k) % 2;
        outStateVectors.clear();
        if (fetchFrom(i, urls[i], kNames[i], centerLat, centerLon, radiusKm, outStateVectors))
            return true;
    }
    outStateVectors.clear();
    return false;
}
