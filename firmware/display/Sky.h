#pragma once
/*
Purpose: The living sky behind the landing, go-around and take-off scenes,
and the light on the map. The sun's height at the airport (worked out from
its position and the clock, no network) sets day, golden hour, twilight or
night; the latest METAR adds cloud, rain, snow, thunder or fog.

  day       blue sky, hazy grey-blue skyline, windows dark
  golden    sun low on the horizon, orange to pink sky
  twilight  deep blue, windows and runway lights coming on
  night     stars and the moon, lit windows, runway edge lights, aircraft
            landing lights and strobes

Unsynced clock: Look::known is false and the scenes keep the classic black
background.
*/
#include <Arduino.h>
#include <time.h>
#include "display/FrameCanvas.h"
#include "utils/Weather.h"

namespace Sky
{
    enum Precip : uint8_t { NoPrecip, Rain, Snow, Thunder };

    struct Look
    {
        bool    known = false;     // clock synced: otherwise the classic black sky
        float   sunElev = -90;     // degrees above the horizon
        float   night = 1;         // 0 = full day .. 1 = full night
        float   golden = 0;        // 0..1: sun near the horizon
        bool    lightsOn = true;   // windows, runway and aircraft lights
        uint8_t cloud = 0;         // 0 clear, 1 scattered, 2 broken, 3 overcast
        Precip  precip = NoPrecip;
        bool    fog = false;
        Rgb     top{0, 0, 0}, horizon{0, 0, 0};
    };

    // Sun elevation (degrees) at lat/lon at a Unix time.
    float sunElevation(double lat, double lon, time_t t);

    // The look at the airport now (or at time t) with this weather.
    Look at(const Metar &m, time_t t);

    // Forced looks for previews: "day", "golden", "twilight", "night",
    // optionally with "+rain", "+snow", "+storm", "+fog", "+overcast".
    Look preview(const char *name);

    // Background above horizonY: gradient, stars or sun or moon, clouds.
    void drawSky(FrameCanvas &c, const Look &l, uint32_t tMs, int horizonY);
    // Rain, snow and fog over the finished scene (before captions).
    void drawWeather(FrameCanvas &c, const Look &l, uint32_t tMs);

    // Colour for scenery silhouettes in this light (night colour given).
    Rgb shade(const Look &l, Rgb nightColour);
    // Multiplier for the map's base layer (dimmer at night).
    float mapLight(const Look &l);
}
