#include "display/MapRenderer.h"
#include "display/Fonts.h"
#include "config/Airport.h"
#include "display/PlaneIcons.h"
#include "display/Font3x5.h"
#include <algorithm>
#include <math.h>

namespace
{
    constexpr float kMaxDeadReckonSec = 45.0f;
    constexpr uint32_t kBlinkMs       = 450;   // on-final blink half-period

    const Rgb kHome  {255, 200,  90};
    const Rgb kLabel { 55,  68,  88};   // kept dim: the map is the subject
    const Rgb kNose  {255, 255, 255};
    const Rgb kSpot  {255, 215, 110};
    constexpr uint32_t kSpotMs  = 2500;   // each aircraft in the spotlight
    constexpr int      kBarY    = 55;     // spotlight bar: the bottom 9 rows

    Rgb lighten(Rgb c, float k)
    {
        return { (uint8_t)(c.r + (255 - c.r) * k), (uint8_t)(c.g + (255 - c.g) * k), (uint8_t)(c.b + (255 - c.b) * k) };
    }

    // A faint line between two map pixels, added to what is there.
    void glowLine(FrameCanvas &c, int x0, int y0, int x1, int y1, Rgb col)
    {
        int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
        for (int guard = 0; guard < 256; ++guard)
        {
            if (x0 >= 0 && y0 >= 0 && x0 < FrameCanvas::W && y0 < FrameCanvas::H)
            {
                const Rgb u = FrameCanvas::unpack(c.get(x0, y0));
                c.set(x0, y0, Rgb{(uint8_t)max((int)u.r, (int)col.r), (uint8_t)max((int)u.g, (int)col.g),
                                  (uint8_t)max((int)u.b, (int)col.b)});
            }
            if (x0 == x1 && y0 == y1) break;
            const int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    }

    // Label boxes already on the map, to keep the next one clear of them.
    // Small (6 bytes): up to 96 of them sit on the display task's stack.
    struct Box { int16_t x, y; uint8_t w, h; };
    bool clear(const Box *boxes, int n, int x, int y, int w, int h)
    {
        if (x < 0 || y < 9 || x + w > FrameCanvas::W || y + h > kBarY) return false;
        for (int i = 0; i < n; ++i)
            if (x < boxes[i].x + boxes[i].w + 1 && boxes[i].x < x + w + 1 &&
                y < boxes[i].y + boxes[i].h + 1 && boxes[i].y < y + h + 1) return false;
        return true;
    }

    void plot(FrameCanvas &c, int x, int y, Rgb col)
    {
        if (x < 0 || y < 0 || x >= FrameCanvas::W || y >= FrameCanvas::H) return;
        c.set(x, y, col);
    }

    bool iconLit(const PlaneIcons::Icon &ic, int x, int y) { return PlaneIcons::lit(ic, x, y); }

    // The icon for an aircraft: its heading, and its size from the type.
    const PlaneIcons::Icon &iconFor(const TrafficPoint &p)
    {
        return PlaneIcons::forHeading(isnan(p.heading) ? 0.0f : p.heading,
                                      (PlaneIcons::Size)(p.size <= 2 ? p.size : 1));
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
        for (int y = -1; y <= ic.n; ++y)
            for (int x = -1; x <= ic.n; ++x)
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
        for (int y = 0; y < ic.n; ++y)
            for (int x = 0; x < ic.n; ++x)
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
        c.print(Fonts::Label, 1, 1, arrivals, kLabel);
        arrW = FrameCanvas::width(Fonts::Label, arrivals);
    }
    if (departures && departures[0] && strcmp(departures, arrivals ? arrivals : "") != 0)
    {
        const int w = FrameCanvas::width(Fonts::Label, departures);
        if (arrW + w + 8 <= FrameCanvas::W)
            c.print(Fonts::Label, FrameCanvas::W - 1 - w, 1, departures, Rgb{200, 140, 50});
    }

    // Trails: where each aircraft has been over the last few minutes, fading
    // towards the oldest point, under the icons.
    for (const TrafficTracker::Tracked &t : traffic.aircraft())
    {
        if (!t.trailN) continue;
        float nx, ny;
        position(t, now, nx, ny);
        int px = (int)lroundf(nx), py = (int)lroundf(ny);
        for (int i = t.trailN - 1; i >= 0; --i)
        {
            int x, y;
            t.trailAt(i, x, y);
            const float k = 0.12f + 0.38f * (i + 1) / t.trailN;   // newest brightest
            glowLine(c, px, py, x, y, FrameCanvas::scale(t.p.colour, k));
            px = x; py = y;
        }
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
            const PlaneIcons::Icon &ic = iconFor(t.p);
            const int ox = (int)lroundf(x) - ic.n / 2, oy = (int)lroundf(y) - ic.n / 2;
            if (pass == 0) iconHalo(c, ox, oy, ic);
            else           iconBody(c, ox, oy, ic, t.p.colour);
        }

    const auto &all = traffic.aircraft();
    if (all.empty()) return;

    // The spotlight: one aircraft at a time (those on final first), ringed,
    // with its details in a bar along the bottom.
    uint8_t order[TrafficTracker::kMax];   // (on the display task's stack: kept small)
    int n = 0;
    for (int i = 0; i < (int)all.size() && n < TrafficTracker::kMax; ++i)
    {
        float x, y;
        position(all[i], now, x, y);
        if (x >= 0 && y >= 0 && x < FrameCanvas::W && y < kBarY) order[n++] = (uint8_t)i;   // on the map only
    }
    if (!n) return;
    std::stable_sort(order, order + n, [&](uint8_t a, uint8_t b) {
        if (all[a].p.onFinal != all[b].p.onFinal) return all[a].p.onFinal;
        return all[a].p.id < all[b].p.id;
    });
    const int spot = order[(now / kSpotMs) % n];
    {
        const TrafficTracker::Tracked &t = all[spot];
        float x, y;
        position(t, now, x, y);
        const float pulse = 0.65f + 0.35f * sinf((now % kSpotMs) / 160.0f);
        for (int d = 0; d < 360; d += 12)
            plot(c, (int)lroundf(x + 6 * cosf(d * 0.01745f)), (int)lroundf(y + 6 * sinf(d * 0.01745f)),
                 FrameCanvas::scale(kSpot, pulse));
        for (int yy = kBarY; yy < FrameCanvas::H; ++yy)
            for (int xx = 0; xx < FrameCanvas::W; ++xx)
                c.set(xx, yy, FrameCanvas::scale(FrameCanvas::unpack(c.get(xx, yy)), 0.18f));
        char line[32], alt[12] = "";
        if (!isnan(t.p.altFt))
        {
            const int a = max(0, (int)lroundf(t.p.altFt / 25.0f) * 25);   // baro reads below 0 on the runway
            if (a >= 1000) snprintf(alt, sizeof(alt), "%d,%03dft", a / 1000, a % 1000);
            else           snprintf(alt, sizeof(alt), "%dft", a);
        }
        char hex[8];
        snprintf(hex, sizeof(hex), "%06lX", (unsigned long)t.p.id);
        snprintf(line, sizeof(line), "%s  %s  %s", t.p.label[0] ? t.p.label : hex, t.p.type, alt);
        c.setClip(0, FrameCanvas::W - 2);
        c.print(Fonts::Label, 2, kBarY + 1, line, kSpot);
        c.resetClip();
    }

    // Everyone else: the flight number in tiny type beside its icon, right,
    // left, above or below, wherever it is clear of the icons and the other
    // labels; left out where there is no room.
    Box boxes[TrafficTracker::kMax * 2];
    int nb = 0;
    for (const TrafficTracker::Tracked &t : all)
    {
        float x, y;
        position(t, now, x, y);
        const int n2 = iconFor(t.p).n;
        boxes[nb++] = Box{(int16_t)(lroundf(x) - n2 / 2), (int16_t)(lroundf(y) - n2 / 2), (uint8_t)n2, (uint8_t)n2};
    }
    for (int k = 0; k < n; ++k)
    {
        const TrafficTracker::Tracked &t = all[order[k]];
        if (order[k] == spot || !t.p.label[0]) continue;
        float fx, fy;
        position(t, now, fx, fy);
        const int x = (int)lroundf(fx), y = (int)lroundf(fy);
        const int w = Font3x5::width(t.p.label);
        const int h = iconFor(t.p).n / 2;   // 2, 3 or 4: labels clear of the icon whatever its size
        const int cand[4][2] = {{x + h + 2, y - 2}, {x - h - 2 - w, y - 2}, {x - w / 2, y - h - 6}, {x - w / 2, y + h + 2}};
        for (const auto &p : cand)
            if (clear(boxes, nb, p[0], p[1], w + 1, 6))
            {
                Font3x5::text(c, p[0], p[1], t.p.label, lighten(t.p.colour, 0.45f));
                if (nb < TrafficTracker::kMax * 2) boxes[nb++] = Box{(int16_t)p[0], (int16_t)p[1], (uint8_t)(w + 1), 6};
                break;
            }
    }
}
