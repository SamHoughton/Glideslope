#include "display/MapRenderer.h"
#include "config/Airport.h"
#include "display/PlaneIcons.h"
#include <math.h>

namespace
{
    constexpr float kMaxDeadReckonSec = 45.0f;
    constexpr uint32_t kBlinkMs       = 450;   // on-final blink half-period

    const Rgb kHome  {255, 200,  90};
    const Rgb kLabel { 55,  68,  88};   // kept dim: the map is the subject
    const Rgb kNose  {255, 255, 255};

    void plot(FrameCanvas &c, int x, int y, Rgb col)
    {
        if (x < 0 || y < 0 || x >= FrameCanvas::W || y >= FrameCanvas::H) return;
        c.set(x, y, col);
    }

    bool iconLit(const PlaneIcons::Icon &ic, int x, int y)
    {
        return x >= 0 && y >= 0 && x < 7 && y < 7 && (ic.rows[y] & (0x40 >> x));
    }

    // Where an aircraft is now: its last report dead-reckoned along the track.
    void position(const TrafficTracker::Tracked &t, unsigned long now, float &x, float &y)
    {
        const TrafficPoint &p = t.p;
        double lat = p.lat, lon = p.lon;
        if (!isnan(p.gsKt) && !isnan(p.heading))
        {
            const float sec = min((now - t.posMs) / 1000.0f, kMaxDeadReckonSec);
            const float km  = p.gsKt * 1.852f * sec / 3600.0f;
            const float hdg = p.heading * (float)M_PI / 180.0f;
            lat += km * cosf(hdg) / 111.2;
            lon += km * sinf(hdg) / (111.32 * cos(p.lat * M_PI / 180.0));
        }
        MapRenderer::project(lat, lon, x, y);
    }

    // Black 1px halo around the icon, so it reads over the river and parks.
    void iconHalo(FrameCanvas &c, int ox, int oy, const PlaneIcons::Icon &ic)
    {
        for (int y = -1; y <= 7; ++y)
            for (int x = -1; x <= 7; ++x)
            {
                if (iconLit(ic, x, y)) continue;
                bool near = false;
                for (int dy = -1; dy <= 1 && !near; ++dy)
                    for (int dx = -1; dx <= 1 && !near; ++dx)
                        near = iconLit(ic, x + dx, y + dy);
                if (near) plot(c, ox + x, oy + y, Rgb{0, 0, 0});
            }
    }

    void iconBody(FrameCanvas &c, int ox, int oy, const PlaneIcons::Icon &ic, Rgb col)
    {
        for (int y = 0; y < 7; ++y)
            for (int x = 0; x < 7; ++x)
                if (iconLit(ic, x, y))
                    plot(c, ox + x, oy + y, (x == ic.noseX && y == ic.noseY) ? kNose : col);
    }
}

void MapRenderer::project(double lat, double lon, float &x, float &y)
{
    x = (float)((lon - g_airport.mapW) / (g_airport.mapE - g_airport.mapW) * FrameCanvas::W);
    y = (float)((g_airport.mapN - lat) / (g_airport.mapN - g_airport.mapS) * FrameCanvas::H);
}

void MapRenderer::render(FrameCanvas &c, const TrafficTracker &traffic, unsigned long now,
                         double homeLat, double homeLon, const char *arrivals, const char *departures,
                         float light)
{
    // Base layer: Heathrow's from flash, or the airport pack's.
    for (int y = 0; y < FrameCanvas::H; ++y)
        for (int x = 0; x < FrameCanvas::W; ++x)
        {
            const uint8_t *p = AirportPack::mapColour(AirportPack::mapIndex(x, y));
            c.set(x, y, FrameCanvas::scale(Rgb{p[0], p[1], p[2]}, light));   // dimmer at night
        }

    // Home: a small warm plus (none when homeLat is NAN, as in the showcase).
    if (!isnan(homeLat))
    {
        float hx, hy;
        project(homeLat, homeLon, hx, hy);
        const int ix = (int)lroundf(hx), iy = (int)lroundf(hy);
        const Rgb dimHome = FrameCanvas::scale(kHome, 0.4f);
        plot(c, ix, iy, kHome);
        plot(c, ix - 1, iy, dimHome); plot(c, ix + 1, iy, dimHome);
        plot(c, ix, iy - 1, dimHome); plot(c, ix, iy + 1, dimHome);
    }

    // Runways in use: arrivals top left, departures top right (dimmer amber).
    int arrW = 0;
    if (arrivals && arrivals[0])
    {
        c.text(1, 1, arrivals, kLabel);
        arrW = FrameCanvas::textWidth(arrivals);
    }
    if (departures && departures[0] && strcmp(departures, arrivals ? arrivals : "") != 0)
    {
        const int w = FrameCanvas::textWidth(departures);
        if (arrW + w + 8 <= FrameCanvas::W)
            c.text(FrameCanvas::W - 1 - w, 1, departures, Rgb{200, 140, 50});
    }

    // Aircraft: a plane icon pointing along the track, in the airline colour
    // with a white nose. Halos first, then icons, so neighbours on the
    // approach don't cut into each other. The aircraft on final blinks.
    const bool blinkOn = (now / kBlinkMs) % 2 == 0;
    for (int pass = 0; pass < 2; ++pass)
        for (const TrafficTracker::Tracked &t : traffic.aircraft())
        {
            if (t.p.onFinal && !blinkOn) continue;
            float x, y;
            position(t, now, x, y);
            const PlaneIcons::Icon &ic = PlaneIcons::forHeading(isnan(t.p.heading) ? 0.0f : t.p.heading);
            const int ox = (int)lroundf(x) - 3, oy = (int)lroundf(y) - 3;
            if (pass == 0) iconHalo(c, ox, oy, ic);
            else           iconBody(c, ox, oy, ic, t.p.colour);
        }
}
