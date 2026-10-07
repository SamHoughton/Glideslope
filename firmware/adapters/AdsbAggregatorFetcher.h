#pragma once
/*
Purpose: Near-real-time ADS-B from community aggregators that share their
data openly without a key: adsb.lol (over plain HTTP, so no TLS handshake
every few seconds) first, adsb.fi (HTTPS) when it fails or rate-limits. Positions
are typically under a second old (OpenSky's are up to 30 s at our polling
rate), and each aircraft carries its type and registration.

  GET http://api.adsb.lol/v2/point/<lat>/<lon>/<radius_nm>
  GET https://opendata.adsb.fi/api/v2/lat/<lat>/lon/<lon>/dist/<radius_nm>

Both return readsb JSON (aircraft under "ac" or "aircraft"). A service that
answers HTTP 429 is rested for a minute. Returns false (so the
caller falls back to OpenSky) when switched off in the settings or when both
services fail.
*/
#include "interfaces/BaseStateVectorFetcher.h"
#include <vector>

class AdsbAggregatorFetcher : public BaseStateVectorFetcher
{
public:
    bool fetchStateVectors(double centerLat, double centerLon, double radiusKm,
                           std::vector<StateVector> &outStateVectors) override;

    // Which service answered last ("adsb.lol", "adsb.fi" or "").
    const char *lastSource() const { return _lastSource; }

    // Shortest gap between fetches adsb.lol currently tolerates. It is
    // learnt: 2 s longer after each rate-limit reply (up to 20 s), 0.5 s
    // shorter after a run of 20 answers (down to zero, i.e. the configured
    // interval). The main loop never fetches faster than this, so adsb.fi
    // (whose TLS handshake costs ~65 KB of heap) is only for real outages.
    unsigned long minIntervalMs() const { return _lolGapMs; }

    // How long to wait before the next fetch rather than fall back to
    // adsb.fi: while adsb.lol is resting after a refusal and answered within
    // the last 90 s, the display dead-reckons and the board simply waits.
    // 0 = fetch now (adsb.lol ready, or genuinely down: then adsb.fi).
    unsigned long holdOffMs() const;

private:
    const char *_lastSource = "";
    unsigned long _restUntil[2] = {0, 0};   // millis() until which a rate-limited service is skipped
    unsigned long _lolGapMs = 0;            // learnt adsb.lol pacing, see minIntervalMs()
    uint8_t       _lolStreak = 0;           // adsb.lol answers since the last change
    unsigned long _lolOkMs = 0;             // when adsb.lol last answered
    std::vector<StateVector> _lolLast;      // its last answer, carried forward while it rests
    bool fetchFrom(int i, const String &url, const char *name, double centerLat, double centerLon,
                   double radiusKm, std::vector<StateVector> &out);
};
