#include "display/Traffic.h"
#include "display/MapRenderer.h"

namespace
{
    constexpr unsigned long kForgetMs = 90000;   // drop aircraft not reported for this long
}

void TrafficTracker::update(const std::vector<TrafficPoint> &points, unsigned long now)
{
    for (const TrafficPoint &p : points)
    {
        if (isnan(p.lat) || isnan(p.lon)) continue;
        Tracked *t = nullptr;
        for (Tracked &e : _tracked)
            if (e.p.id == p.id) { t = &e; break; }

        if (!t)
        {
            if ((int)_tracked.size() >= kMax) continue;
            _tracked.push_back(Tracked());
            t = &_tracked.back();
        }
        t->p     = p;
        t->posMs = now;
        // A trail point every kTrailMs, where it is on the map now.
        if (!t->trailMs || now - t->trailMs >= kTrailMs)
        {
            float x, y;
            MapRenderer::project(p.lat, p.lon, x, y);
            if (x >= 0 && y >= 0 && x < FrameCanvas::W && y < FrameCanvas::H)
            {
                t->trail[t->trailHead][0] = (uint8_t)lroundf(x);
                t->trail[t->trailHead][1] = (uint8_t)lroundf(y);
                t->trailHead = (t->trailHead + 1) % kTrail;
                if (t->trailN < kTrail) ++t->trailN;
            }
            t->trailMs = now;
        }
    }

    for (auto it = _tracked.begin(); it != _tracked.end(); )
        it = (now - it->posMs > kForgetMs) ? _tracked.erase(it) : std::next(it);
}
