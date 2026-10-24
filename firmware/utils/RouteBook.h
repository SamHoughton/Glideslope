#pragma once
/*
Purpose: The routes the board has learned, kept in flash (8.3), for when the
online lookups come up empty: VIR156T, Las Vegas to Heathrow almost daily,
is unknown to adsb.im and only sometimes reached at hexdb; once learned, it
is known for good.

  /routes.txt   one line a route: "VIR156T KLAS EGLL"; a later line for the
                same call sign wins (a route that changed)
  /routes.old   the previous file, once routes.txt passes 96 KB (about
                5,000 routes); looked in second

Online first, always: the book only fills in when every source failed, so a
call sign that moves to another route is corrected the next time it is
looked up. Main loop only. No RAM kept: each look-up reads the file.
*/
#include <Arduino.h>

namespace RouteBook
{
    // origin and dest: room for 5 (ICAO + terminator). False if never learned.
    bool find(const char *callsign, char *origin, char *dest);
    void remember(const char *callsign, const char *origin, const char *dest);
}
