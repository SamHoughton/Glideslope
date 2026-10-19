#pragma once
/*
Purpose: Home Assistant over MQTT: the board's sensors announced with MQTT
discovery (retained), their values as one JSON state message every 30 s,
and an availability topic with a last will, so Home Assistant shows the
board offline when it goes away.

  homeassistant/sensor/glideslope_<id>/<key>/config   discovery (retained)
  glideslope/<id>/state                               {"next":"BA117",...}
  glideslope/<id>/status                              online / offline

Off while mqtt_host is blank. Plain TCP (port 1883 by default); a
connection attempt at most once a minute, and only with heap to spare.
*/
#include <Arduino.h>

namespace HomeAssistant
{
struct State
{
    char  next[10] = "";       // next arrival, "" if none
    int   nextEtaSec = -1;     // -1 if none
    char  arrRunways[12] = "", depRunways[12] = "";
    int   arrivals = 0, departures = 0, holding = 0;
    char  last[32] = "";       // the latest card: "BA117 JFK>LHR"
    char  spotted[32] = "";    // the latest overhead / watchlist aircraft
};

// Main loop, every pass: keeps the connection and publishes when due.
void loop(const State &s);
}
