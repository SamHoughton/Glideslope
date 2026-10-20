#pragma once

#include <Arduino.h>
#include <vector>
#include <map>
#include "interfaces/BaseStateVectorFetcher.h"
#include "interfaces/BaseFlightFetcher.h"
#include "interfaces/BaseLogoStore.h"
#include "models/StateVector.h"
#include "models/FlightInfo.h"

// "BAW487E" -> "BA487E" (IATA flight number from an airline call sign);
// empty when the airline is unknown.
String flightNumberFromCallsign(const String &callsign);

struct CachedFlightEntry
{
    String           key;            // call sign; "" = a free slot
    FlightInfo       info;
    unsigned long    expiryMs = 0;
};

class FlightDataFetcher
{
public:
    // logoStore may be nullptr — logo lookup is simply skipped.
    FlightDataFetcher(BaseStateVectorFetcher *stateFetcher,
                      BaseFlightFetcher      *flightFetcher,
                      BaseLogoStore          *logoStore = nullptr);

    size_t fetchFlights(std::vector<StateVector> &outStates,
                        std::vector<FlightInfo>  &outFlights);

    // False when the last fetch got no answer from any position source.
    bool lastFetchOk() const { return _lastFetchOk; }

private:
    bool _lastFetchOk = true;
    BaseStateVectorFetcher *_stateFetcher;
    BaseFlightFetcher      *_flightFetcher;
    BaseLogoStore          *_logoStore;

    // Keyed by trimmed callsign; stores enriched static data between fetch cycles
    // A fixed table, made once with the fetcher (8.0): a map allocated a
    // ~350-byte node for every new route and freed it later, scattering
    // holes through the heap until a TLS handshake no longer fitted.
    // Reusing slots keeps the Strings' buffers too.
    CachedFlightEntry _flightCache[8];
    CachedFlightEntry *cacheFind(const String &callsign);
    static constexpr size_t kMaxCachedFlights = 8;   // ~1 KB each; with "nearest only" few are re-shown

public:
    size_t cachedFlights() const
    {
        size_t n = 0;
        for (const CachedFlightEntry &e : _flightCache) n += e.key.length() > 0;
        return n;
    }
};
