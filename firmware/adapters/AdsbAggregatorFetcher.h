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

class AdsbAggregatorFetcher : public BaseStateVectorFetcher
{
public:
    bool fetchStateVectors(double centerLat, double centerLon, double radiusKm,
                           std::vector<StateVector> &outStateVectors) override;

    // Which service answered last ("adsb.lol", "adsb.fi" or "").
    const char *lastSource() const { return _lastSource; }

private:
    const char *_lastSource = "";
    unsigned long _restUntil[2] = {0, 0};   // millis() until which a rate-limited service is skipped
    bool fetchFrom(int i, const String &url, const char *name, double centerLat, double centerLon,
                   double radiusKm, std::vector<StateVector> &out);
};
