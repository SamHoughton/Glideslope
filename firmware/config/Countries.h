#pragma once
/*
Purpose: The country an airport is in, from its ICAO code's prefix (9.1: the
panel's "new sticker" for a country). Generated from the sticker book's
table in web/index.html by tools/gen_stickers.py, so the two agree.
*/
#include <Arduino.h>

namespace Countries
{
    // ISO code ("GB") and English name of an ICAO airport code's country;
    // false (and "") when its prefix isn't known.
    bool of(const char *icao, char iso[3], const char **name);
}
