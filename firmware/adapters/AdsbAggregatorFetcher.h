#pragma once
/*
Purpose: Near-real-time ADS-B from community aggregators that share their
data openly without a key: adsb.fi first, over one HTTPS connection kept
open between fetches, and adsb.lol as the backup when adsb.fi fails or
refuses (since October 2026 adsb.lol allows 500 requests a day per address,
so it can't carry a fetch every 5 s). Positions are typically under a
second old, and each aircraft carries its type and registration.

  GET https://opendata.adsb.fi/api/v2/lat/<lat>/lon/<lon>/dist/<radius_nm>
  GET https://api.adsb.lol/v2/point/<lat>/<lon>/<radius_nm>

Both return readsb JSON (aircraft under "aircraft" or "ac"), read one
aircraft at a time (adsb.fi's chunked replies decoded on the fly). A service
that refuses is rested; returns false (so the caller falls back to OpenSky)
when switched off in the settings or when both services fail.
*/
#include "interfaces/BaseStateVectorFetcher.h"
#include <vector>

class AdsbAggregatorFetcher : public BaseStateVectorFetcher
{
public:
    bool fetchStateVectors(double centerLat, double centerLon, double radiusKm,
                           std::vector<StateVector> &outStateVectors) override;

    // Which service answered last ("adsb.fi", "adsb.lol" or "").
    const char *lastSource() const { return _lastSource; }

    // Shortest gap between fetches adsb.lol currently tolerates. It is
    // learnt: 2 s longer after each rate-limit reply (up to 20 s), 0.5 s
    // shorter after a run of 20 answers (down to zero, i.e. the configured
    // interval). The main loop never fetches faster than this, so adsb.fi
    // (whose TLS handshake costs ~65 KB of heap) is only for real outages.
    unsigned long minIntervalMs() const { return _gapMs; }

    // How long to wait before the next fetch rather than fall back to the
    // backup: while the primary is resting after a refusal and answered
    // within the last 90 s, the display dead-reckons and the board waits.
    // 0 = fetch now (primary ready, or genuinely down: then the backup).
    unsigned long holdOffMs() const;

    // A small extra area from the primary (holding stacks): only when it is
    // ready, never the backup. Leaves the pacing and lastSource as they were.
    bool fetchArea(double lat, double lon, double radiusKm, std::vector<StateVector> &out);

    // Close the kept-open connection (frees ~45 KB); it reopens on
    // the next fetch. Registered with HeapWatch, so other TLS users call it.
    void closeSecure();

private:
    const char *_lastSource = "";
    unsigned long _restUntil[2] = {0, 0};   // millis() until which a rate-limited service is skipped
    unsigned long _gapMs = 0;            // learnt primary pacing, see minIntervalMs()
    uint8_t       _streak = 0;           // primary answers since the last change
    unsigned long _primaryOkMs = 0;             // when the primary last answered
    unsigned long _carriedMs = 0;           // time the positions in the caller's list are for
    std::vector<StateVector> _spare;   // the other of the two lists that take turns (see fetchStateVectors)
    class WiFiClientSecure *_keepTls = nullptr;   // the primary's connection, kept open (HTTP keep-alive)
    class HTTPClient       *_keepHttp = nullptr;
    bool fetchFrom(int i, const String &url, const char *name, double centerLat, double centerLon,
                   double radiusKm, std::vector<StateVector> &out, bool mayRetry = true);
};
