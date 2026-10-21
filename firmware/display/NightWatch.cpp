#include "display/NightWatch.h"
#include "display/InfoScreens.h"
#include "display/LandingScene.h"
#include "display/Font3x5.h"
#include <math.h>

namespace
{
    constexpr float kHalfFov = 60.0f;        // degrees either side of the way the window faces
    constexpr float kMaxDeadReckonSec = 20.0f;

    float wrap180(float d)
    {
        while (d > 180) d -= 360;
        while (d < -180) d += 360;
        return d;
    }

    void dot(FrameCanvas &c, int x, int y, Rgb col, float k)
    {
        if (x < 0 || y < 0 || x >= FrameCanvas::W || y >= FrameCanvas::H) return;
        const Rgb under = FrameCanvas::unpack(c.get(x, y));
        c.set(x, y, Rgb{(uint8_t)max((int)under.r, (int)(col.r * k)), (uint8_t)max((int)under.g, (int)(col.g * k)),
                        (uint8_t)max((int)under.b, (int)(col.b * k))});
    }
}

void NightWatch::render(FrameCanvas &c, unsigned long now, const Sky::Look &sky, const char *weather,
                        const TrafficTracker &traffic, double homeLat, double homeLon, float facingDeg,
                        const Tracker &t)
{
    // The night scene, without its make-believe aircraft: the real ones follow.
    InfoScreens::renderNightScene(c, now, weather, sky, false);
    const int horizon = LandingScene::kHorizonY;

    const double r = M_PI / 180.0;
    for (const TrafficTracker::Tracked &a : traffic.aircraft())
    {
        const TrafficPoint &p = a.p;
        if (isnan(p.lat) || isnan(p.lon) || isnan(p.altFt) || p.altFt < 150) continue;   // on the ground
        // Where it is now: its report carried on along its track.
        double lat = p.lat, lon = p.lon;
        if (!isnan(p.gsKt) && !isnan(p.heading))
        {
            const float sec = min((now - a.posMs) / 1000.0f, kMaxDeadReckonSec);
            const float km = p.gsKt * 1.852f * sec / 3600.0f;
            lat += km * cosf(p.heading * (float)r) / 111.2;
            lon += km * sinf(p.heading * (float)r) / (111.32 * cos(p.lat * r));
        }
        // Bearing and distance from home, and its angle above the horizon.
        const double dLat = (lat - homeLat) * 111.2, dLon = (lon - homeLon) * 111.32 * cos(homeLat * r);
        const double km = sqrt(dLat * dLat + dLon * dLon);
        const float bearing = (float)fmod(atan2(dLon, dLat) / r + 360.0, 360.0);
        const float rel = wrap180(bearing - facingDeg);
        if (fabsf(rel) > kHalfFov) continue;   // behind the window frame
        const float elev = (float)(atan2(p.altFt * 0.0003048, max(km, 0.2)) / r);
        const int x = (int)lroundf(FrameCanvas::W / 2.0f + rel / kHalfFov * (FrameCanvas::W / 2.0f - 2));
        // Up the panel by the square root of its angle: the distant ones a few
        // degrees up show just above the rooftops, not lost among them, and
        // one overhead still reaches the top.
        const int y = max(2, horizon - 4 - (int)lroundf(sqrtf(max(0.0f, elev)) * 7.0f));
        // Nearer is brighter; far ones are faint points over the rooftops.
        const float k = max(0.6f, min(1.0f, 14.0f / (float)(km + 4.0)));
        dot(c, x, y, Rgb{255, 250, 228}, k);
        // The nav light on the side we see: moving left across the view, its
        // left wing (red) is towards us; moving right, the green one.
        if (!isnan(p.heading))
        {
            const float across = sinf((p.heading - facingDeg) * (float)r);   // > 0: moving right in the view
            dot(c, x + (across > 0 ? -1 : 1), y, across > 0 ? Rgb{60, 220, 90} : Rgb{255, 50, 40}, k * 0.8f);
        }
        // The anti-collision beacon, each at its own moment.
        if ((now + p.id * 37) % 1200 < 110) dot(c, x, y - 1, Rgb{255, 40, 30}, k);
    }

    // The ground: one row of distant runway lights under the skyline, then
    // the tracker, dim, two lines in turn.
    for (int y = horizon + 1; y < FrameCanvas::H; ++y)
        for (int x = 0; x < FrameCanvas::W; ++x) c.set(x, y, Rgb{6, 8, 12});
    const float lit = sky.lightsOn ? 0.5f : 0.2f;
    for (int x = 3; x < FrameCanvas::W; x += 5) c.set(x, horizon + 1, FrameCanvas::scale(Rgb{200, 160, 80}, lit));
    char line[40];
    struct tm lt;
    char last[8] = "--:--", next[8] = "";
    if (t.last) { localtime_r(&t.last, &lt); snprintf(last, sizeof(last), "%02d:%02d", lt.tm_hour, lt.tm_min); }
    if (t.next) { localtime_r(&t.next, &lt); snprintf(next, sizeof(next), "%02d:%02d", lt.tm_hour, lt.tm_min); }
    if ((now / 6000) % 2 == 0 || !next[0])
    {
        if (t.last) snprintf(line, sizeof(line), "TONIGHT %d   LAST %s", t.tonight, last);
        else        snprintf(line, sizeof(line), "TONIGHT %d   TODAY %d", t.tonight, t.today);   // none since a restart
    }
    else
        snprintf(line, sizeof(line), "NEXT %s   TODAY %d", next, t.today);
    Font3x5::text(c, (FrameCanvas::W - Font3x5::width(line)) / 2, horizon + 2, line, Rgb{80, 88, 112}, false);
}
