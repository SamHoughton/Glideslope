#include "display/Traffic.h"

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
        else if (t->p.lat != p.lat || t->p.lon != p.lon)
        {
            // Moved: push the previous position onto the trail.
            for (int i = kTrail - 1; i > 0; --i)
            {
                t->trailLat[i] = t->trailLat[i - 1];
                t->trailLon[i] = t->trailLon[i - 1];
            }
            t->trailLat[0] = t->p.lat;
            t->trailLon[0] = t->p.lon;
            if (t->trailN < kTrail) ++t->trailN;
        }
        t->p     = p;
        t->posMs = now;
    }

    for (auto it = _tracked.begin(); it != _tracked.end(); )
        it = (now - it->posMs > kForgetMs) ? _tracked.erase(it) : std::next(it);
}
