#pragma once

#include <Arduino.h>
#include <time.h>
#include <vector>
#include "AirportInfo.h"

struct FlightInfo
{
    // Flight identifiers
    String ident;
    String ident_icao;
    String ident_iata;

    // Operator
    String operator_code;
    String operator_icao;
    String operator_iata;

    // Route
    AirportInfo origin;
    AirportInfo destination;

    // Aircraft
    String aircraft_code;
    String registration;      // tail number / civil registration, e.g. "G-EUPX"

    // Human-friendly display strings
    String airline_display_name_full;
    String aircraft_display_name_short;

    // Airline logo as RGB565 pixels (AirlineLogo::WIDTH * HEIGHT entries); empty if unavailable
    std::vector<uint16_t> airline_logo_rgb565;

    // Flight parameters (display-ready units: ft, kts, deg, fpm)
    double baro_altitude = NAN;
    double velocity = NAN;
    double heading = NAN;
    double vertical_rate = NAN;
    double distance_km = NAN;
    double bearing_deg = NAN;
    double lat = NAN;
    double lon = NAN;

    // Scheduled runway arrival (Unix time, UTC); 0 when unknown. Only AeroAPI
    // provides it — used for "4 MIN EARLY" on the landed card.
    time_t scheduled_on = 0;
};

namespace AirlineLogo
{
    static constexpr uint8_t WIDTH  = 32;
    static constexpr uint8_t HEIGHT = 32;
}
