#include "adapters/AdsbAggregatorFetcher.h"
#include "adapters/ReadsbParser.h"
#include "utils/ChunkedStream.h"
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
    // At most this many aircraft per fetch, reserved once (11 nm round
    // Heathrow is usually 10-20): a growing vector needs a second, larger
    // block while the first is still held.
    constexpr size_t   kMaxAircraft = 24;
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
    if (!_keepHttp && !_keepTls) return;
    if (_keepHttp) { _keepHttp->end(); delete _keepHttp; _keepHttp = nullptr; }
    if (_keepTls)  { _keepTls->stop(); delete _keepTls; _keepTls = nullptr; }
    Log.println("AdsbAggregatorFetcher: feed connection closed (room for another TLS request)");
}

bool AdsbAggregatorFetcher::fetchFrom(int i, const String &url, const char *name, double centerLat,
                                      double centerLon, double radiusKm, std::vector<StateVector> &out,
                                      bool mayRetry)
{
    if (_restUntil[i] && (long)(millis() - _restUntil[i]) < 0)
        return false;
    _restUntil[i] = 0;
    if (!s_self) { s_self = this; g_tlsReleaser = releaseTls; }

    // Both services are HTTPS only. A TLS handshake needs ~60 KB of heap at
    // its peak, too much every few seconds, so the primary's connection
    // (adsb.fi) is opened once and kept open (HTTP/1.1 keep-alive). The
    // backup (adsb.lol, 500 requests a day) gets a one-off connection.
    std::unique_ptr<WiFiClientSecure> oneOff;
    std::unique_ptr<HTTPClient>       oneOffHttp;
    HTTPClient *httpp;
    if (i == 0)
    {
        if (!_keepTls)
        {
            if (!tlsAffordable(name)) return false;
            _keepTls = new WiFiClientSecure;
            _keepTls->setInsecure();   // public open data; matches the project's other HTTPS clients
            _keepHttp = new HTTPClient;
            _keepHttp->setReuse(true);
            Log.println("AdsbAggregatorFetcher: opening the adsb.fi connection");
        }
        httpp = _keepHttp;
        if (!httpp->begin(*_keepTls, url)) { closeSecure(); return false; }
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
        httpp->useHTTP10(false);   // adsb.lol refuses HTTP/1.0 (426); chunked replies are decoded below
    }
    HTTPClient &http = *httpp;
    static const char *kHeaders[] = {"Transfer-Encoding"};
    http.collectHeaders(kHeaders, 1);
    http.setTimeout(kTimeoutMs);
    http.setUserAgent("Glideslope/1.0 (+https://github.com/SamHoughton/Glideslope)");

    // Room for the list first, before anything is sent (see below); only
    // when it has to grow (it keeps its room from one fetch to the next).
    if (out.capacity() < kMaxAircraft && ESP.getMaxAllocHeap() < kMaxAircraft * sizeof(StateVector) + 2048)
    {
        Log.println("AdsbAggregatorFetcher: not enough memory for the list, skipping this fetch");
        if (i != 0) http.end();
        return false;
    }
    // Claimed now, while the check above still holds: the request's TLS
    // buffers come next (reserving after the GET failed once).
    out.reserve(kMaxAircraft);
    const bool reused = i == 0 && _keepHttp && _keepTls && _keepTls->connected();
    const int code = http.GET();
    if (code < 0 && reused && mayRetry)
    {
        // The server closed the kept-open connection while it was idle: one
        // more try on a fresh one, rather than resting the service for 30 s.
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
                const unsigned long base = max(_gapMs, (unsigned long)g_config.local_fetch_interval_seconds * 1000UL);
                _gapMs  = min(base + 2000UL, 20000UL);
                _streak = 0;
            }
            Log.printf("AdsbAggregatorFetcher: %s rate-limited, resting it for %lu s (fetches now at least %lu s apart)\n",
                       name, kBackoffMs / 1000, _gapMs / 1000);
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

    // One aircraft at a time from the stream, then the rest of the reply is
    // read so the kept-open connection starts clean for the next request.
    Stream &body = http.getStream();
    // adsb.fi lists aircraft under "aircraft", adsb.lol under "ac". adsb.fi
    // answers HTTP/1.1 chunked: decoded on the fly, and read to its end so
    // the kept-open connection starts clean.
    const char *key = strcmp(name, "adsb.lol") == 0 ? "ac" : "aircraft";
    const bool chunked = http.header("Transfer-Encoding").equalsIgnoreCase("chunked");
    ChunkedStream dechunk(body);
    Stream &in = chunked ? static_cast<Stream &>(dechunk) : body;
    const int n = ReadsbParser::parseStream(in, key, centerLat, centerLon, radiusKm, out);
    if (chunked) dechunk.finish();
    else if (n >= 0) body.find((char *)"}");   // after the array: flat fields, then the closing brace
    http.end();
    if (n < 0)
    {
        Log.printf("AdsbAggregatorFetcher: %s returned no aircraft list\n", name);
        if (i == 0) closeSecure();
        out.clear();
        return false;
    }
    _lastSource = name;
    if (i == 0) _primaryOkMs = millis();
    if (i == 0 && _gapMs && ++_streak >= 20)
    {
        _gapMs -= min(_gapMs, 500UL);
        if (_gapMs <= (unsigned long)g_config.local_fetch_interval_seconds * 1000UL) _gapMs = 0;
        _streak = 0;
    }
    return true;
}

bool AdsbAggregatorFetcher::fetchArea(double lat, double lon, double radiusKm, std::vector<StateVector> &out)
{
    if (!g_config.use_community_feeds) return false;
    if (_restUntil[0] && (long)(millis() - _restUntil[0]) < 0) return false;
    const int nm = max(1, (int)ceil(radiusKm / 1.852));
    char url[80];
    snprintf(url, sizeof(url), "https://opendata.adsb.fi/api/v2/lat/%.4f/lon/%.4f/dist/%d", lat, lon, nm);
    const char *source = _lastSource;
    const bool ok = fetchFrom(0, String(url), "adsb.fi", lat, lon, radiusKm, out);
    _lastSource = source;
    return ok;
}

unsigned long AdsbAggregatorFetcher::holdOffMs() const
{
    const unsigned long now = millis();
    if (!_restUntil[0] || (long)(now - _restUntil[0]) >= 0) return 0;   // primary ready
    if (!_primaryOkMs || now - _primaryOkMs > 90000UL) return 0;               // down for a while: use the backup
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
    // adsb.fi first, over a connection kept open between fetches; adsb.lol
    // only as the backup: since October 2026 it allows 500 requests a day
    // per address (and 60 a minute), about 40 minutes of fetches every 5 s.
    const String urls[2] = {
        String("https://opendata.adsb.fi/api/v2/lat/") + lat + "/lon/" + lon + "/dist/" + nm,
        String("https://api.adsb.lol/v2/point/") + lat + "/" + lon + "/" + nm,
    };
    static const char *const kNames[2] = { "adsb.fi", "adsb.lol" };

    // Two lists take turns: the caller's (the previous fetch's aircraft, kept
    // for carrying forward) goes to _spare, and the one set aside last time
    // comes back emptied but with its room. Nothing large is allocated per
    // fetch: a new list every 5 s fragmented the heap until a TLS handshake
    // found no room (5.1's soak).
    _spare.swap(outStateVectors);
    outStateVectors.clear();

    // The primary first (adsb.fi, kept-open connection).
    if (fetchFrom(0, urls[0], kNames[0], centerLat, centerLon, radiusKm, outStateVectors))
    {
        _carriedMs = millis();
        return true;
    }

    // Primary resting or refused, but it answered recently: carry its last
    // answer forward (each aircraft moved along its track and climb rate to
    // now) rather than pay for the backup's TLS handshake (~65 KB of heap).
    const unsigned long now = millis();
    if (_primaryOkMs && now - _primaryOkMs <= 90000UL && !_spare.empty() && _carriedMs)
    {
        const double dt = (now - _carriedMs) / 1000.0;
        _carriedMs = now;
        outStateVectors.swap(_spare);
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
        _lastSource = "adsb.fi (carried forward)";
        return true;
    }

    // Primary down for a while: the backup (adsb.lol), if there is room for TLS.
    outStateVectors.clear();
    if (tlsAffordable("adsb.lol") && fetchFrom(1, urls[1], kNames[1], centerLat, centerLon, radiusKm, outStateVectors))
    {
        _carriedMs = millis();
        return true;
    }
    outStateVectors.clear();
    return false;
}
