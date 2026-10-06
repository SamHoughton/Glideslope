#include "display/ApproachModel.h"
#include <math.h>

namespace
{
    // Heathrow (EGLL) runway thresholds. Positions are approximate (within ~100 m);
    // good enough to pick a runway and estimate time to touchdown.
    struct Threshold { const char *name; double lat, lon; float course; };
    const Threshold kThresholds[] = {
        {"27R", 51.4777, -0.4332, 269.7f},   // north runway, east end, landing westbound
        {"27L", 51.4649, -0.4340, 269.7f},   // south runway, east end
        {"09L", 51.4775, -0.4850,  89.7f},   // north runway, west end, landing eastbound
        {"09R", 51.4647, -0.4826,  89.7f},   // south runway, west end
    };
    constexpr double kAirportLat = 51.4700, kAirportLon = -0.4543;

    constexpr float kApproachMaxKm   = 40.0f;   // beyond this it's "inbound", not on final
    constexpr float kApproachMaxFt   = 8000.0f;
    constexpr float kAlignedDeg      = 30.0f;   // track within this of the runway course
    constexpr float kStripRangeKm    = 25.0f;   // distance mapped onto the progress strip

    float angleDiff(float a, float b)
    {
        float d = fmodf(a - b + 540.0f, 360.0f) - 180.0f;
        return fabsf(d);
    }

    // Local flat-earth offset in km of (lat, lon) from (lat0, lon0): x east, y north.
    void offsetKm(double lat0, double lon0, double lat, double lon, float &x, float &y)
    {
        x = (float)((lon - lon0) * cos(lat0 * M_PI / 180.0) * 111.32);
        y = (float)((lat - lat0) * 110.574);
    }

    bool isEgll(const AirportInfo &a) { return a.code_icao == "EGLL" || a.code_iata == "LHR"; }

    float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
}

ApproachStatus ApproachModel::evaluate(const FlightInfo &f)
{
    ApproachStatus s;
    if (isnan(f.lat) || isnan(f.lon))
        return s;

    float ax, ay;
    offsetKm(kAirportLat, kAirportLon, f.lat, f.lon, ax, ay);
    const float airportKm = sqrtf(ax * ax + ay * ay);

    const bool  arriving  = isEgll(f.destination);
    const bool  departing = isEgll(f.origin);
    const float alt       = isnan(f.baro_altitude) ? NAN : (float)f.baro_altitude;
    const float vr        = isnan(f.vertical_rate) ? 0.0f : (float)f.vertical_rate;
    const float gsKt      = isnan(f.velocity) ? NAN : (float)f.velocity;
    const float track     = isnan(f.heading) ? NAN : (float)f.heading;

    // Arrival on final: track lined up with a runway, ahead of its threshold, low enough.
    // Without route data, a low descending aircraft lined up counts too.
    const bool maybeArrival = arriving || (!departing && !isnan(alt) && alt < 4000 && vr < -300);
    if (maybeArrival && !isnan(track) && (isnan(alt) || alt < kApproachMaxFt))
    {
        const Threshold *best = nullptr;
        float bestAlong = 0, bestCross = 1e9f;
        for (const Threshold &t : kThresholds)
        {
            if (angleDiff(track, t.course) > kAlignedDeg)
                continue;
            float x, y;
            offsetKm(t.lat, t.lon, f.lat, f.lon, x, y);
            // Distance still to fly along the runway course (positive = not yet at threshold).
            const float crs   = t.course * (float)M_PI / 180.0f;
            const float along = -(x * sinf(crs) + y * cosf(crs));
            const float cross = fabsf(x * cosf(crs) - y * sinf(crs));
            if (along < -0.5f || along > kApproachMaxKm)
                continue;
            if (cross > 1.5f + along * 0.15f)     // allow a wider funnel further out
                continue;
            if (cross < bestCross) { best = &t; bestAlong = along; bestCross = cross; }
        }
        if (best)
        {
            strlcpy(s.runway, best->name, sizeof(s.runway));
            s.distKm   = bestAlong < 0 ? 0 : bestAlong;
            s.progress = clamp01(1.0f - s.distKm / kStripRangeKm);
            const bool down = bestAlong < 0.3f || (!isnan(alt) && alt < 150);
            s.phase  = down ? ApproachStatus::Landing : ApproachStatus::Approach;
            if (!isnan(gsKt) && gsKt > 30)
                s.etaSec = s.distKm * 1000.0f / (gsKt * 0.514444f);
            else if (down)
                s.etaSec = 0;
            return s;
        }
    }

    s.distKm   = airportKm;
    s.progress = clamp01(1.0f - airportKm / kStripRangeKm);

    if (arriving)
    {
        s.phase = ApproachStatus::Inbound;
        return s;
    }

    if (departing)
    {
        s.phase = ApproachStatus::Outbound;
        // Departure runway: the runway whose far end the aircraft is beyond,
        // picked by which centreline it is closest to.
        if (!isnan(track) && airportKm < 20 && vr > 200)
        {
            const bool westbound = angleDiff(track, 270) <= 45;
            const bool eastbound = angleDiff(track, 90) <= 45;
            if (westbound || eastbound)
            {
                const bool north = ay > (float)((51.4712 - kAirportLat) * 110.574); // midway between runways
                const char *rwy = westbound ? (north ? "27R" : "27L") : (north ? "09L" : "09R");
                strlcpy(s.runway, rwy, sizeof(s.runway));
                s.phase = ApproachStatus::Departed;
            }
        }
        return s;
    }

    s.phase = ApproachStatus::Overflight;
    return s;
}

ApproachStatus ApproachModel::advance(const ApproachStatus &s, double gsKt, uint32_t ageMs)
{
    ApproachStatus out = s;
    if ((s.phase != ApproachStatus::Approach && s.phase != ApproachStatus::Landing) || isnan(s.distKm))
        return out;
    const float flownKm = isnan(gsKt) ? 0.0f : (float)gsKt * 0.514444f * ageMs / 1.0e6f;
    out.distKm   = max(0.0f, s.distKm - flownKm);
    out.progress = clamp01(1.0f - out.distKm / kStripRangeKm);
    if (!isnan(s.etaSec))
        out.etaSec = max(0.0f, s.etaSec - ageMs / 1000.0f);
    return out;
}

void ApproachModel::label(const ApproachStatus &s, char *buf, size_t len)
{
    switch (s.phase)
    {
        case ApproachStatus::Approach:   snprintf(buf, len, "APPROACH %s", s.runway); break;
        case ApproachStatus::Landing:    snprintf(buf, len, "LANDING %s", s.runway);  break;
        case ApproachStatus::Inbound:    snprintf(buf, len, "INBOUND LHR");           break;
        case ApproachStatus::Departed:   snprintf(buf, len, "DEPARTED %s", s.runway); break;
        case ApproachStatus::Outbound:   snprintf(buf, len, "DEPARTED LHR");          break;
        case ApproachStatus::Overflight: snprintf(buf, len, "OVERFLIGHT");            break;
        default:                         if (len) buf[0] = '\0';                      break;
    }
}
