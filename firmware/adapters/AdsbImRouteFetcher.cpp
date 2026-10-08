#include "adapters/AdsbImRouteFetcher.h"
#include "adapters/FlightWallFetcher.h"
#include "utils/TelnetLogger.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>

namespace
{
    // "EPWA-EGLL" -> "EPWA", "EGLL" (multi-leg "A-B-C": first and last).
    bool splitPair(const char *s, String &from, String &to)
    {
        if (!s || !strchr(s, '-') || !strcmp(s, "unknown")) return false;
        const char *first = strchr(s, '-'), *last = strrchr(s, '-');
        from = String(s).substring(0, first - s);
        to   = String(last + 1);
        return from.length() && to.length();
    }
}

bool AdsbImRouteFetcher::fetchFlightInfo(const String &callsign, const String &icao24, FlightInfo &outInfo)
{
    (void)icao24;
    outInfo.ident = callsign;
    if (_restUntil && (long)(millis() - _restUntil) < 0) return false;
    _restUntil = 0;

    WiFiClient client;
    HTTPClient http;
    if (!http.begin(client, "http://adsb.im/api/0/routeset")) return false;
    http.setTimeout(5000);
    http.setUserAgent("Glideslope (+https://github.com/SamHoughton/Glideslope)");
    http.addHeader("Content-Type", "application/json");
    char body[64];
    snprintf(body, sizeof(body), "{\"planes\":[{\"callsign\":\"%s\"}]}", callsign.c_str());
    const int code = http.POST((uint8_t *)body, strlen(body));
    if (code != 200)
    {
        http.end();
        _restUntil = millis() + 60000;
        Log.printf("AdsbImRouteFetcher: HTTP %d, resting it for a minute\n", code);
        return false;
    }
    JsonDocument filter;
    filter[0]["airline_code"] = true;
    filter[0]["airport_codes"] = true;
    filter[0]["_airport_codes_iata"] = true;
    filter[0]["plausible"] = true;
    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    http.end();
    if (err) { Log.printf("AdsbImRouteFetcher: JSON error %s\n", err.c_str()); return false; }

    JsonObject r = doc[0];
    String fromIcao, toIcao, fromIata, toIata;
    if (r.isNull() || !splitPair(r["airport_codes"] | "", fromIcao, toIcao))
    {
        Log.printf("AdsbImRouteFetcher: no route for %s\n", callsign.c_str());
        return false;
    }
    splitPair(r["_airport_codes_iata"] | "", fromIata, toIata);
    outInfo.origin.code_icao = fromIcao;
    outInfo.destination.code_icao = toIcao;
    outInfo.origin.code_iata = fromIata;
    outInfo.destination.code_iata = toIata;
    const char *airline = r["airline_code"] | "";
    if (strlen(airline) == 3 && strcmp(airline, "unknown") != 0)
    {
        outInfo.operator_icao = airline;
        FlightWallFetcher fw;
        String name;
        if (fw.getAirlineName(outInfo.operator_icao, name) && name.length())
            outInfo.airline_display_name_full = name;
    }
    Log.printf("AdsbImRouteFetcher: %s %s>%s%s\n", callsign.c_str(),
               (fromIata.length() ? fromIata : fromIcao).c_str(), (toIata.length() ? toIata : toIcao).c_str(),
               (r["plausible"] | true) ? "" : " (implausible)");
    return true;
}
