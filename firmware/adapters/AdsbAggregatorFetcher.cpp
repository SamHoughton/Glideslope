#include "adapters/AdsbAggregatorFetcher.h"
#include "adapters/ReadsbParser.h"
#include "config/RuntimeConfig.h"
#include "utils/TelnetLogger.h"
#include "utils/GeoUtils.h"
#include "utils/HeapWatch.h"
#include <math.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <memory>
#include <ArduinoJson.h>

namespace
{
    constexpr uint32_t kTimeoutMs = 6000;
    constexpr uint32_t kBackoffMs = 30000;   // rest a service for half a minute after HTTP 429
    constexpr uint32_t kErrorRestMs = 30000; // and half a minute after a timeout or other error
}

namespace
{
    AdsbAggregatorFetcher *s_self = nullptr;
    void releaseTls() { if (s_self) s_self->closeSecure(); }
}

void AdsbAggregatorFetcher::closeSecure()
{
    if (!_lolHttp && !_lolTls) return;
    if (_lolHttp) { _lolHttp->end(); delete _lolHttp; _lolHttp = nullptr; }
    if (_lolTls)  { _lolTls->stop(); delete _lolTls; _lolTls = nullptr; }
    Log.println("AdsbAggregatorFetcher: adsb.lol connection closed (room for another TLS request)");
}

bool AdsbAggregatorFetcher::fetchFrom(int i, const String &url, const char *name, double centerLat,
                                      double centerLon, double radiusKm, std::vector<StateVector> &out,
                                      bool mayRetry)
{
    if (_restUntil[i] && (long)(millis() - _restUntil[i]) < 0)
        return false;
    _restUntil[i] = 0;
    if (!s_self) { s_self = this; g_tlsReleaser = releaseTls; }

    // Both services are HTTPS only (adsb.lol stopped answering plain HTTP in
    // October 2026). A TLS handshake needs ~60 KB of heap at its peak, too
    // much every few seconds, so adsb.lol's connection is opened once and
    // kept open (HTTP/1.1 keep-alive; it sends Content-Length, so the body
    // streams into the parser). adsb.fi, only used while adsb.lol is down,
    // gets a one-off connection.
    std::unique_ptr<WiFiClientSecure> oneOff;
    std::unique_ptr<HTTPClient>       oneOffHttp;
    HTTPClient *httpp;
    if (i == 0)
    {
        if (!_lolTls)
        {
            if (!tlsAffordable(name)) return false;
            _lolTls = new WiFiClientSecure;
            _lolTls->setInsecure();   // public open data; matches the project's other HTTPS clients
            _lolHttp = new HTTPClient;
            _lolHttp->setReuse(true);
            Log.println("AdsbAggregatorFetcher: opening the adsb.lol connection");
        }
        httpp = _lolHttp;
        if (!httpp->begin(*_lolTls, url)) { closeSecure(); return false; }
        httpp->useHTTP10(false);
    }
    else
    {
        closeSecure();
        if (!tlsAffordable(name)) return false;
        oneOff.reset(new WiFiClientSecure);
        oneOff->setInsecure();
        oneOffHttp.reset(new HTTPClient);
        httpp = oneOffHttp.get();
        if (!httpp->begin(*oneOff, url)) return false;
        httpp->useHTTP10(true);   // no chunked encoding, so the body streams straight into the parser
    }
    HTTPClient &http = *httpp;
    http.setTimeout(kTimeoutMs);
    http.setUserAgent("Glideslope/1.0 (+https://github.com/SamHoughton/Glideslope)");

    const bool reused = i == 0 && _lolHttp && _lolTls && _lolTls->connected();
    const int code = http.GET();
    if (code < 0 && reused && mayRetry)
    {
        // The server closed the kept-open connection while it was idle: one
        // more try on a fresh one, rather than resting adsb.lol for 30 s.
        http.end();
        closeSecure();
        return fetchFrom(i, url, name, centerLat, centerLon, radiusKm, out, false);
    }
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
        if (i == 0) closeSecure();
        return false;
    }

    // One aircraft at a time from the stream (adsb.lol lists them under "ac",
    // adsb.fi under "aircraft"), then the rest of the reply is read so the
    // kept-open connection starts clean for the next request.
    Stream &body = http.getStream();
    const int n = ReadsbParser::parseStream(body, i == 0 ? "ac" : "aircraft", centerLat, centerLon, radiusKm, out);
    if (n >= 0) body.find((char *)"}");   // after the array: a few flat fields, then the closing brace
    http.end();
    if (n < 0)
    {
        Log.printf("AdsbAggregatorFetcher: %s returned no aircraft list\n", name);
        if (i == 0) closeSecure();
        out.clear();
        return false;
    }
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

bool AdsbAggregatorFetcher::fetchArea(double lat, double lon, double radiusKm, std::vector<StateVector> &out)
{
    if (!g_config.use_community_feeds) return false;
    if (_restUntil[0] && (long)(millis() - _restUntil[0]) < 0) return false;
    const int nm = max(1, (int)ceil(radiusKm / 1.852));
    char url[80];
    snprintf(url, sizeof(url), "https://api.adsb.lol/v2/point/%.4f/%.4f/%d", lat, lon, nm);
    const char *source = _lastSource;
    const bool ok = fetchFrom(0, String(url), "adsb.lol", lat, lon, radiusKm, out);
    _lastSource = source;
    return ok;
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
        String("https://api.adsb.lol/v2/point/") + lat + "/" + lon + "/" + nm,
        String("https://opendata.adsb.fi/api/v2/lat/") + lat + "/lon/" + lon + "/dist/" + nm,
    };
    static const char *const kNames[2] = { "adsb.lol", "adsb.fi" };

    // The caller's list holds the previous fetch's aircraft. Set it aside
    // (a swap: nothing copied; with 40 aircraft a copy was ~9 KB of heap).
    std::vector<StateVector> previous;
    previous.swap(outStateVectors);

    // adsb.lol first (plain HTTP, light on memory).
    if (fetchFrom(0, urls[0], kNames[0], centerLat, centerLon, radiusKm, outStateVectors))
    {
        _carriedMs = millis();
        return true;
    }

    // adsb.lol resting or refused, but it answered recently: carry its last
    // answer forward (each aircraft moved along its track and climb rate to
    // now) rather than pay for adsb.fi's TLS handshake (~65 KB of heap).
    const unsigned long now = millis();
    if (_lolOkMs && now - _lolOkMs <= 90000UL && !previous.empty() && _carriedMs)
    {
        const double dt = (now - _carriedMs) / 1000.0;
        _carriedMs = now;
        outStateVectors.swap(previous);
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
    {
        _carriedMs = millis();
        return true;
    }
    outStateVectors.clear();
    return false;
}
