#pragma once
/*
Purpose: Routes from adsb.im's open route database (the same one tar1090 and
adsb.lol use), over plain HTTP: no TLS handshake, so no 60 KB heap dip for
every new flight.

  POST http://adsb.im/api/0/routeset   {"planes":[{"callsign":"BAW845"}]}
  -> [{"callsign":"BAW845","airline_code":"BAW","airport_codes":"EPWA-EGLL",
       "_airport_codes_iata":"WAW-LHR","plausible":true, ...}]

Fills origin and destination (ICAO and IATA), the operator and the airline's
name (FlightWall CDN, also plain HTTP). Registration and type come from the
live feed. Returns false when the route is unknown, so the next source in
the chain (hexdb, over HTTPS) is only asked for flights adsb.im lacks.
*/
#include "interfaces/BaseFlightFetcher.h"

class AdsbImRouteFetcher : public BaseFlightFetcher
{
public:
    bool fetchFlightInfo(const String &callsign, const String &icao24, FlightInfo &outInfo) override;

private:
    unsigned long _restUntil = 0;   // after an error, leave it alone for a minute
};
