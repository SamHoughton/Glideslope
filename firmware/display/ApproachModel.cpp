#include "display/ApproachModel.h"
#include "config/Airport.h"
#include <math.h>

namespace
{

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


    float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
}

ApproachStatus ApproachModel::evaluate(const FlightInfo &f)
{
    ApproachStatus s;
    if (isnan(f.lat) || isnan(f.lon))
        return s;

    float ax, ay;
    offsetKm(g_airport.lat, g_airport.lon, f.lat, f.lon, ax, ay);
    const float airportKm = sqrtf(ax * ax + ay * ay);

    const bool  arriving  = AirportPack::isHome(f.destination);
    const bool  departing = AirportPack::isHome(f.origin);
    const float alt       = isnan(f.baro_altitude) ? NAN : (float)f.baro_altitude;
    const float vr        = isnan(f.vertical_rate) ? 0.0f : (float)f.vertical_rate;
    const float gsKt      = isnan(f.velocity) ? NAN : (float)f.velocity;
    const float track     = isnan(f.heading) ? NAN : (float)f.heading;

    // Arrival on final: track lined up with a runway, ahead of its threshold, low enough.
    // Without route data, a low descending aircraft lined up counts too.
    const bool maybeArrival = arriving || (!departing && !isnan(alt) && alt < 4000 && vr < -300);
    if (maybeArrival && !isnan(track) && (isnan(alt) || alt < kApproachMaxFt))
    {
        const RunwayEnd *best = nullptr;
        float bestAlong = 0, bestCross = 1e9f;
        for (int k = 0; k < g_airport.runwayCount; ++k)
        {
            const RunwayEnd &t = g_airport.runways[k];
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
        // Departure runway: climbing out along a runway's extended centreline,
        // in that runway's direction and ahead of its threshold (it took off
        // from that end). The closest centreline wins.
        if (!isnan(track) && airportKm < 20 && vr > 200)
        {
            const RunwayEnd *best = nullptr;
            float bestCross = 1e9f;
            for (int k = 0; k < g_airport.runwayCount; ++k)
            {
                const RunwayEnd &t = g_airport.runways[k];
                if (angleDiff(track, t.course) > 45) continue;
                float x, y;
                offsetKm(t.lat, t.lon, f.lat, f.lon, x, y);
                const float crs   = t.course * (float)M_PI / 180.0f;
                const float along = x * sinf(crs) + y * cosf(crs);          // ahead of the threshold
                const float cross = fabsf(x * cosf(crs) - y * sinf(crs));
                if (along < 0 || along > 20) continue;
                if (cross > 2.0f + along * 0.25f) continue;
                if (cross < bestCross) { best = &t; bestCross = cross; }
            }
            if (best)
            {
                strlcpy(s.runway, best->name, sizeof(s.runway));
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
    // Over the threshold by dead reckoning: "LANDING", until a report confirms it.
    if (out.distKm < 0.3f)
        out.phase = ApproachStatus::Landing;
    return out;
}

bool ApproachModel::climbingOut(const FlightInfo &f, char *runway, size_t len)
{
    if (isnan(f.lat) || isnan(f.lon) || isnan(f.heading) || isnan(f.vertical_rate) || f.vertical_rate < 300)
        return false;
    const RunwayEnd *best = nullptr;
    float bestCross = 1e9f;
    for (int k = 0; k < g_airport.runwayCount; ++k)
    {
        const RunwayEnd &t = g_airport.runways[k];
        if (angleDiff((float)f.heading, t.course) > 30) continue;
        float x, y;
        offsetKm(t.lat, t.lon, f.lat, f.lon, x, y);
        const float crs   = t.course * (float)M_PI / 180.0f;
        const float along = x * sinf(crs) + y * cosf(crs);          // ahead of the threshold
        const float cross = fabsf(x * cosf(crs) - y * sinf(crs));
        if (along < 1.0f || along > 15.0f) continue;               // past the runway, up to 15 km out
        if (cross > 1.5f + along * 0.2f) continue;
        if (cross < bestCross) { best = &t; bestCross = cross; }
    }
    if (!best) return false;
    if (runway && len) strlcpy(runway, best->name, len);
    return true;
}

void ApproachModel::label(const ApproachStatus &s, char *buf, size_t len)
{
    switch (s.phase)
    {
        case ApproachStatus::Approach:   snprintf(buf, len, "APPROACH %s", s.runway); break;
        case ApproachStatus::Landing:    snprintf(buf, len, "LANDING %s", s.runway);  break;
        case ApproachStatus::Inbound:    snprintf(buf, len, "INBOUND %s", g_airport.iata[0] ? g_airport.iata : g_airport.icao); break;
        case ApproachStatus::Departed:   snprintf(buf, len, "DEPARTED %s", s.runway); break;
        case ApproachStatus::Outbound:   snprintf(buf, len, "DEPARTED %s", g_airport.iata[0] ? g_airport.iata : g_airport.icao); break;
        case ApproachStatus::Overflight: snprintf(buf, len, "OVERFLIGHT");            break;
        default:                         if (len) buf[0] = '\0';                      break;
    }
}
