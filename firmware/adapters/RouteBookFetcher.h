#pragma once
/*
Purpose: The route book (utils/RouteBook.h) as a link in the route chain
(9.0): after adsb.im (plain HTTP), before the sources that need a secure
connection (hexdb, OpenSky, AeroAPI). A regular whose route adsb.im lacks
(VIR156T) is then answered from flash, without the TLS handshake that each
of those lookups costs: the handshakes are where the heap goes lowest.
Answers only the route; registration and type come with the positions.
*/
#include "interfaces/BaseFlightFetcher.h"
#include "utils/RouteBook.h"
#include "config/Airports.h"
#include "utils/TelnetLogger.h"

class RouteBookFetcher : public BaseFlightFetcher
{
public:
    // online: asked when the book doesn't know the call sign (it hands over
    // itself, so no scratch FlightInfo is kept as a FallbackFlightFetcher's is).
    explicit RouteBookFetcher(BaseFlightFetcher *online) : _online(online) {}

    bool fetchFlightInfo(const String &callsign, const String &icao24, FlightInfo &outInfo) override
    {
        char o[5], d[5], iata[4];
        if (!RouteBook::find(callsign.c_str(), o, d))
            return _online ? _online->fetchFlightInfo(callsign, icao24, outInfo) : false;
        outInfo.origin.code_icao = o;
        outInfo.destination.code_icao = d;
        outInfo.origin.code_iata = Airports::icaoToIata(o, iata) ? iata : "";
        outInfo.destination.code_iata = Airports::icaoToIata(d, iata) ? iata : "";
        Log.printf("RouteBook: %s %s>%s from flash\n", callsign.c_str(), o, d);
        return true;
    }

private:
    BaseFlightFetcher *_online;
};
