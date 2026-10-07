#include "display/MapRenderer.h"
#include "display/MapBase.h"
#include <math.h>

namespace
{
    constexpr float kMaxDeadReckonSec = 45.0f;
    constexpr uint32_t kBlinkMs       = 450;   // on-final blink half-period

    const Rgb kHome  {255, 200,  90};
    const Rgb kLabel { 90, 110, 140};

    // Plot with additive-style "max" blending, so trails don't erase each other.
    void plot(FrameCanvas &c, int x, int y, Rgb col)
    {
        if (x < 0 || y < 0 || x >= FrameCanvas::W || y >= FrameCanvas::H) return;
        const Rgb cur = FrameCanvas::unpack(c.get(x, y));
        c.set(x, y, Rgb{ max(cur.r, col.r), max(cur.g, col.g), max(cur.b, col.b) });
    }

    // Dotted, so a trail never reads as a solid map feature like the Thames.
    void segment(FrameCanvas &c, float x0, float y0, float x1, float y1, Rgb col)
    {
        const int n = (int)ceilf(max(fabsf(x1 - x0), fabsf(y1 - y0)));
        for (int i = 0; i <= n; i += 2)
        {
            const float t = n ? (float)i / n : 0.0f;
            plot(c, (int)lroundf(x0 + (x1 - x0) * t), (int)lroundf(y0 + (y1 - y0) * t), col);
        }
    }
}

void MapRenderer::project(double lat, double lon, float &x, float &y)
{
    x = (float)((lon - MapBase::LON_W) / (MapBase::LON_E - MapBase::LON_W) * FrameCanvas::W);
    y = (float)((MapBase::LAT_N - lat) / (MapBase::LAT_N - MapBase::LAT_S) * FrameCanvas::H);
}

void MapRenderer::render(FrameCanvas &c, const TrafficTracker &traffic, unsigned long now,
                         double homeLat, double homeLon, const char *runwayInUse)
{
    // Base layer straight from flash.
    for (int y = 0; y < FrameCanvas::H; ++y)
    {
        const char *row = MapBase::kRows[y];
        for (int x = 0; x < FrameCanvas::W; ++x)
        {
            const int idx = row[x] - '0';
            const uint8_t *p = MapBase::kPalette[idx];
            c.set(x, y, Rgb{p[0], p[1], p[2]});
        }
    }

    // Home: a small warm plus.
    float hx, hy;
    project(homeLat, homeLon, hx, hy);
    const int ix = (int)lroundf(hx), iy = (int)lroundf(hy);
    plot(c, ix, iy, kHome);
    const Rgb dimHome = FrameCanvas::scale(kHome, 0.4f);
    plot(c, ix - 1, iy, dimHome); plot(c, ix + 1, iy, dimHome);
    plot(c, ix, iy - 1, dimHome); plot(c, ix, iy + 1, dimHome);

    // Aircraft: fading trail through the reported positions, then the
    // dead-reckoned current position as a bright dot.
    for (const TrafficTracker::Tracked &t : traffic.aircraft())
    {
        const TrafficPoint &p = t.p;
        float x, y;
        project(p.lat, p.lon, x, y);

        // Dead-reckon from the last report along the track.
        if (!isnan(p.gsKt) && !isnan(p.heading))
        {
            const float sec = min((now - t.posMs) / 1000.0f, kMaxDeadReckonSec);
            const float km  = p.gsKt * 1.852f * sec / 3600.0f;
            const float hdg = p.heading * (float)M_PI / 180.0f;
            const double lat = p.lat + km * cosf(hdg) / 111.2;
            const double lon = p.lon + km * sinf(hdg) / (111.32 * cos(p.lat * M_PI / 180.0));
            project(lat, lon, x, y);
        }

        float px = x, py = y;
        for (int i = 0; i < t.trailN; ++i)
        {
            float tx, ty;
            project(t.trailLat[i], t.trailLon[i], tx, ty);
            const float k = 0.45f * (1.0f - (float)i / TrafficTracker::kTrail);
            segment(c, px, py, tx, ty, FrameCanvas::scale(p.colour, k));
            px = tx; py = ty;
        }

        const bool lit = !p.onFinal || (now / kBlinkMs) % 2 == 0;
        if (lit)
        {
            const int dx = (int)lroundf(x), dy = (int)lroundf(y);
            plot(c, dx, dy, p.colour);
            // On final: a slightly larger dot so it stands out.
            if (p.onFinal) { plot(c, dx + 1, dy, p.colour); plot(c, dx, dy + 1, p.colour); plot(c, dx + 1, dy + 1, p.colour); }
        }
    }

    // Runway in use, top left (open country to the north-west).
    if (runwayInUse && runwayInUse[0])
    {
        char label[12];
        snprintf(label, sizeof(label), "LHR %s", runwayInUse);
        c.text(1, 1, label, kLabel);
    }
}
