/*
Purpose: Orchestrate fetching and enrichment of flight data for display.
Flow:
1) Use BaseStateVectorFetcher to fetch nearby state vectors by geo filter.
2) For every callsign, fetch enrichment data (hexdb primary, AeroAPI fallback).
3) Filter: discard flights without both origin and destination (GA, unidentified).
4) Sort aircraft on final approach first (closest to touchdown), then by
   distance; optionally trim to the first one only.
Output: Returns displayable flight count; fills outStates/outFlights.
*/
#include <algorithm>
#include "core/FlightDataFetcher.h"
#include "config/RuntimeConfig.h"
#include "adapters/FlightWallFetcher.h"
#include "utils/TelnetLogger.h"
#include "display/ApproachModel.h"
#include "config/AirlineCodes.h"
#include "utils/HeapWatch.h"

namespace
{
    // ICAO airline designator from a callsign: the leading three letters of
    // "BAW123A"; empty for registrations such as "GABCD" or "N123AB".
    String callsignAirline(const String &callsign)
    {
        if (callsign.length() < 4) return String();
        for (int i = 0; i < 3; ++i)
            if (!isalpha((unsigned char)callsign[i])) return String();
        if (!isdigit((unsigned char)callsign[3])) return String();
        String code = callsign.substring(0, 3);
        code.toUpperCase();
        return code;
    }

    // Sort key: aircraft on final approach to a Heathrow runway come first,
    // ordered by distance to touchdown; everything else after, by distance.
    float flightPriority(const FlightInfo &f)
    {
        const ApproachStatus s = ApproachModel::evaluate(f);
        const bool landing = s.phase == ApproachStatus::Approach || s.phase == ApproachStatus::Landing;
        const float d = isnan(f.distance_km) ? 999.0f : (float)f.distance_km;
        return landing && !isnan(s.distKm) ? s.distKm : 1000.0f + d;
    }

    // Same, from a raw state vector (before enrichment, so no route is known;
    // ApproachModel then relies on a low, descending aircraft lined up).
    float statePriority(const StateVector &s)
    {
        FlightInfo f;
        f.lat           = s.lat;
        f.lon           = s.lon;
        f.heading       = s.heading;
        f.distance_km   = s.distance_km;
        f.baro_altitude = isnan(s.baro_altitude) ? NAN : s.baro_altitude * 3.28084;
        f.velocity      = isnan(s.velocity) ? NAN : s.velocity * 1.94384;
        f.vertical_rate = isnan(s.vertical_rate) ? NAN : s.vertical_rate * 196.85;
        return flightPriority(f);
    }

    // Sub-fleets that fly in a parent airline's livery and have no logo file of their own.
    String logoAlias(const String &icao)
    {
        static const struct { const char *from, *to; } kAliases[] = {
            {"SHT", "BAW"},   // BA Shuttle
            {"EFW", "BAW"},   // BA Euroflyer
            {"CFE", "BAW"},   // BA CityFlyer
            {"NAX", "NOZ"},   // Norwegian
            {"NSZ", "NOZ"},
            {"GWI", "EWG"},   // Eurowings
        };
        for (const auto &a : kAliases)
            if (icao == a.from) return a.to;
        return String();
    }
}

// "BAW487E" -> "BA487E"; "EZY73CA" -> "U2 73CA" (space when the IATA code
// ends in a digit, so it doesn't run into the number). Empty if the
// airline is unknown, leaving the card on the raw call sign.
String flightNumberFromCallsign(const String &callsign)
{
    const String icao = callsignAirline(callsign);
    if (icao.length() == 0) return String();
    const char *iata = airlineIata(icao.c_str());
    if (!iata) return String();
    String suffix = callsign.substring(3);
    suffix.trim();
    while (suffix.length() > 1 && suffix[0] == '0' && isdigit((unsigned char)suffix[1]))
        suffix.remove(0, 1);                         // BAW0123 -> BA123
    const bool gap = isdigit((unsigned char)iata[1]) && suffix.length() && isdigit((unsigned char)suffix[0]);
    return String(iata) + (gap ? " " : "") + suffix;
}

FlightDataFetcher::FlightDataFetcher(BaseStateVectorFetcher *stateFetcher,
                                     BaseFlightFetcher      *flightFetcher,
                                     BaseLogoStore          *logoStore)
    : _stateFetcher(stateFetcher), _flightFetcher(flightFetcher), _logoStore(logoStore) {}

size_t FlightDataFetcher::fetchFlights(std::vector<StateVector> &outStates,
                                       std::vector<FlightInfo> &outFlights)
{
    outStates.clear();
    outFlights.clear();

    bool ok = _stateFetcher->fetchStateVectors(
        g_config.center_lat,
        g_config.center_lon,
        g_config.radius_km,
        outStates);
    _lastFetchOk = ok;
    heapCheckpoint("position fetch");
    if (!ok)
        return 0;

    // Evict stale cache entries so RAM doesn't accumulate across long uptimes
    const unsigned long nowMs = millis();
    for (auto it = _flightCache.begin(); it != _flightCache.end(); )
        it = (nowMs >= it->second.expiryMs) ? _flightCache.erase(it) : std::next(it);

    const unsigned long cacheTtlMs =
        (unsigned long)g_config.aeroapi_cache_ttl_seconds * 1000UL;


    // Aircraft on final approach first (closest to touchdown first), then the
    // rest by distance, so the hexdb budget — and "nearest only" — goes to the
    // next plane to land.
    std::stable_sort(outStates.begin(), outStates.end(),
        [](const StateVector &a, const StateVector &b) {
            return statePriority(a) < statePriority(b);
        });

    // Returns true for ICAO commercial flight numbers: 2-3 letter airline code
    // (e.g. BAW, EZY, AA) immediately followed by a digit at position 2 or 3.
    // Filters out registrations (GBIZG, N1234X) and all-alpha squawks that
    // hexdb will never have route data for.
    auto isCommercialCallsign = [](const String &cs) -> bool {
        if (cs.length() < 3) return false;
        for (int i = 0; i < (int)cs.length(); ++i) {
            if (isdigit((unsigned char)cs[i]))
                return i >= 2 && i <= 3;
            if (i >= 3) break;
        }
        return false;
    };

    FlightWallFetcher fw;
    const size_t totalStates = outStates.size();
    size_t stateIdx = 0;
    for (const StateVector &s : outStates)
    {
        ++stateIdx;
        if (s.callsign.length() == 0)
            continue;

        if (!isCommercialCallsign(s.callsign))
        {
            Log.printf("FlightDataFetcher: [%u/%u] skipping %s (non-commercial)\n",
                       stateIdx, totalStates, s.callsign.c_str());
            continue;
        }

        FlightInfo info;

        auto it = _flightCache.find(s.callsign);
        if (it != _flightCache.end() && nowMs < it->second.expiryMs)
        {
            // Cache hit — reuse static enrichment data (logos are NOT cached,
            // see below — they are reloaded from LittleFS on every cycle)
            info = it->second.info;
            Log.printf("FlightDataFetcher: [%u/%u] cache hit for %s\n",
                       stateIdx, totalStates, s.callsign.c_str());
        }
        else
        {
            // Cache miss — fetch from hexdb (primary) / AeroAPI (fallback)
            Log.printf("FlightDataFetcher: [%u/%u] fetching %s\n",
                       stateIdx, totalStates, s.callsign.c_str());
            const bool fetchOk = _flightFetcher->fetchFlightInfo(s.callsign, s.icao24, info);
            heapCheckpoint("route lookup");

            if (info.ident.length() == 0)
                info.ident = s.callsign;

            // Resolve human-readable aircraft type name (e.g. "A320neo") from CDN.
            // Airline name and airport IATA codes come from hexdb directly.
            if (info.aircraft_code.length())
            {
                String aircraftShort, aircraftFull;
                const bool named = fw.getAircraftName(info.aircraft_code, aircraftShort, aircraftFull);
                heapCheckpoint("aircraft name lookup");
                if (named)
                    if (aircraftShort.length())
                        info.aircraft_display_name_short = aircraftShort;
            }

            // Only cache when AeroAPI responded successfully.  Transient failures
            // (network error, timeout, truncated JSON) are NOT cached so the next
            // cycle retries the flight fresh.  A successful 200 with no route data
            // (GA / unknown) uses the shorter fail-TTL to avoid repeated lookups.
            if (fetchOk)
            {
                const bool hasRoute = info.origin.code_icao.length() > 0 &&
                                      info.destination.code_icao.length() > 0;
                const unsigned long ttlMs = hasRoute
                    ? cacheTtlMs
                    : (unsigned long)g_config.aeroapi_fail_cache_ttl_seconds * 1000UL;
                CachedFlightEntry entry;
                entry.info     = info;       // airline_logo_rgb565 is empty here — good
                entry.expiryMs = nowMs + ttlMs;
                _flightCache[s.callsign] = entry;
            }
            else
            {
                Log.printf("FlightDataFetcher: transient error for %s — skipping cache, will retry\n",
                           s.callsign.c_str());
            }
        }

        // Load logo fresh from LittleFS on every cycle (cache hit or miss).
        // This keeps logo pixels out of the heap-resident cache and avoids the
        // fragmentation that causes MBEDTLS_ERR_SSL_ALLOC_FAILED (-32512).
        // The callsign prefix names the airline actually flying the flight; the
        // hexdb operator code is the aircraft's registered owner, which is
        // wrong for leased and sub-fleet aircraft, so it is only a fallback.
        if (_logoStore)
        {
            const String prefix = callsignAirline(s.callsign);
            if (info.operator_icao.length() == 0)
                info.operator_icao = prefix;
            const String candidates[] = { prefix, logoAlias(prefix), info.operator_icao };
            for (const String &code : candidates)
                if (code.length() && _logoStore->getAirlineLogo(code, info.airline_logo_rgb565))
                    break;
        }

        // Flight-number style ident for the card: the API's IATA flight number
        // when it has one (AeroAPI), else the call sign with its ICAO airline
        // prefix swapped for the IATA code (BAW487E -> BA487E), else the call sign.
        if (info.ident_iata.length() == 0)
            info.ident_iata = flightNumberFromCallsign(s.callsign);

        // Always apply live telemetry from the current StateVector
        if (!isnan(s.baro_altitude)) info.baro_altitude  = s.baro_altitude * 3.28084;
        if (!isnan(s.velocity))      info.velocity       = s.velocity      * 1.94384;
        if (!isnan(s.heading))       info.heading        = s.heading;
        if (!isnan(s.vertical_rate)) info.vertical_rate  = s.vertical_rate * 196.85;
        info.distance_km = s.distance_km;
        info.bearing_deg = s.bearing_deg;
        info.lat         = s.lat;
        info.lon         = s.lon;

        // Apply local ADS-B enrichment as fallback — only fills gaps left by
        // AeroAPI (or when AeroAPI is not configured / unreachable).
        // AeroAPI values always take priority since they are more authoritative.
        if (info.registration.length() == 0 && s.registration.length() > 0)
            info.registration = s.registration;
        if (info.aircraft_code.length() == 0 && s.aircraft_type.length() > 0)
            info.aircraft_code = s.aircraft_type;

        outFlights.push_back(info);

        // Early exit when showing only the nearest flight — stop as soon as we
        // have one routed result so distant aircraft don't consume hexdb calls.
        if (g_config.display_nearest_only)
        {
            const bool hasRoute = info.origin.code_icao.length() > 0 &&
                                  info.destination.code_icao.length() > 0;
            if (hasRoute)
            {
                Log.printf("FlightDataFetcher: nearest routed flight (%s) — stopping early\n",
                           s.callsign.c_str());
                break;
            }
        }
    }

    // Filter: discard flights without a complete route (general aviation, unidentified)
    const size_t beforeFilter = outFlights.size();
    outFlights.erase(
        std::remove_if(outFlights.begin(), outFlights.end(),
            [](const FlightInfo &f) {
                return f.origin.code_icao.length() == 0 ||
                       f.destination.code_icao.length() == 0;
            }),
        outFlights.end());
    Log.printf("FlightDataFetcher: %u flights before filter, %u after\n",
               (unsigned)beforeFilter, (unsigned)outFlights.size());

    // Landing traffic first, then nearest
    std::stable_sort(outFlights.begin(), outFlights.end(),
        [](const FlightInfo &a, const FlightInfo &b) {
            return flightPriority(a) < flightPriority(b);
        });

    // Trim to nearest if configured
    if (g_config.display_nearest_only && !outFlights.empty())
        outFlights.resize(1);

    return outFlights.size();
}
