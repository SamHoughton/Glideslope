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
        t->p     = p;
        t->posMs = now;
    }

    for (auto it = _tracked.begin(); it != _tracked.end(); )
        it = (now - it->posMs > kForgetMs) ? _tracked.erase(it) : std::next(it);
}
