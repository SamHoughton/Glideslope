#include "adapters/AdsbAggregatorFetcher.h"
#include "adapters/ReadsbParser.h"
#include "config/RuntimeConfig.h"
#include "utils/TelnetLogger.h"
#include "utils/GeoUtils.h"
#include "utils/HeapWatch.h"
#include <math.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

namespace
{
    constexpr uint32_t kTimeoutMs = 6000;
    constexpr uint32_t kBackoffMs = 30000;   // rest a service for half a minute after HTTP 429
    constexpr uint32_t kErrorRestMs = 30000; // and half a minute after a timeout or other error
}

bool AdsbAggregatorFetcher::fetchFrom(int i, const String &url, const char *name, double centerLat,
                                      double centerLon, double radiusKm, std::vector<StateVector> &out)
{
    if (_restUntil[i] && (long)(millis() - _restUntil[i]) < 0)
        return false;
    _restUntil[i] = 0;

    // adsb.lol is fetched over plain HTTP: no TLS handshake (40-50 KB of heap
    // at its peak) every few seconds. adsb.fi only answers over HTTPS.
    const bool tls = url.startsWith("https://");
    WiFiClient       plain;
    WiFiClientSecure secure;
    if (tls) secure.setInsecure();   // public open data; matches the project's other HTTPS clients
    HTTPClient http;
    if (!http.begin(tls ? static_cast<WiFiClient &>(secure) : plain, url))
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
            if (i == 0)
            {
                const unsigned long base = max(_lolGapMs, (unsigned long)g_config.local_fetch_interval_seconds * 1000UL);
                _lolGapMs  = min(base + 2000UL, 20000UL);
                _lolStreak = 0;
            }
            Log.printf("AdsbAggregatorFetcher: %s rate-limited, resting it for %lu s (fetches now at least %lu s apart)\n",
                       name, kBackoffMs / 1000, _lolGapMs / 1000);
        }
        else
        {
            // Timeouts and errors too: a short rest, so each fetch doesn't
            // wait out the 6 s timeout before trying the other service.
            _restUntil[i] = millis() + kErrorRestMs;
            Log.printf("AdsbAggregatorFetcher: %s HTTP %d, resting it for 30 s\n", name, code);
        }
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
    if (i == 0) _lolOkMs = millis();
    if (i == 0 && _lolGapMs && ++_lolStreak >= 20)
    {
        _lolGapMs -= min(_lolGapMs, 500UL);
        if (_lolGapMs <= (unsigned long)g_config.local_fetch_interval_seconds * 1000UL) _lolGapMs = 0;
        _lolStreak = 0;
    }
    return true;
}

unsigned long AdsbAggregatorFetcher::holdOffMs() const
{
    const unsigned long now = millis();
    if (!_restUntil[0] || (long)(now - _restUntil[0]) >= 0) return 0;   // adsb.lol ready
    if (!_lolOkMs || now - _lolOkMs > 90000UL) return 0;               // down for a while: use adsb.fi
    return _restUntil[0] - now;
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
        String("http://api.adsb.lol/v2/point/") + lat + "/" + lon + "/" + nm,
        String("https://opendata.adsb.fi/api/v2/lat/") + lat + "/lon/" + lon + "/dist/" + nm,
    };
    static const char *const kNames[2] = { "adsb.lol", "adsb.fi" };

    // adsb.lol first (plain HTTP, light on memory).
    outStateVectors.clear();
    if (fetchFrom(0, urls[0], kNames[0], centerLat, centerLon, radiusKm, outStateVectors))
    {
        _lolLast = outStateVectors;
        return true;
    }

    // adsb.lol resting or refused, but it answered recently: carry its last
    // answer forward (each aircraft moved along its track and climb rate to
    // now) rather than pay for adsb.fi's TLS handshake (~65 KB of heap).
    const unsigned long now = millis();
    if (_lolOkMs && now - _lolOkMs <= 90000UL && !_lolLast.empty())
    {
        const double dt = (now - _lolOkMs) / 1000.0;
        outStateVectors = _lolLast;
        for (StateVector &s : outStateVectors)
        {
            if (!isnan(s.velocity) && !isnan(s.heading))
            {
                const double m = s.velocity * dt, h = s.heading * M_PI / 180.0;
                s.lat += m * cos(h) / 111320.0;
                s.lon += m * sin(h) / (111320.0 * cos(s.lat * M_PI / 180.0));
            }
            if (!isnan(s.vertical_rate) && !isnan(s.baro_altitude))
                s.baro_altitude = max(0.0, s.baro_altitude + s.vertical_rate * dt);
            s.distance_km = haversineKm(centerLat, centerLon, s.lat, s.lon);
            s.bearing_deg = computeBearingDeg(centerLat, centerLon, s.lat, s.lon);
        }
        _lastSource = "adsb.lol (carried forward)";
        return true;
    }

    // adsb.lol down for a while: adsb.fi (HTTPS), if there is room for TLS.
    outStateVectors.clear();
    if (tlsAffordable("adsb.fi") && fetchFrom(1, urls[1], kNames[1], centerLat, centerLon, radiusKm, outStateVectors))
        return true;
    outStateVectors.clear();
    return false;
}
