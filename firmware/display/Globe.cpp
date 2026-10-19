#include "display/Globe.h"
#include "display/LandMask.h"
#include <math.h>

namespace
{
constexpr float kDeg = (float)M_PI / 180.0f;
constexpr int   kCx = 31, kCy = 32, kR = 30;   // the globe: left half of the panel
constexpr int   kTextX = 67;                   // the column beside it (10 characters)

struct V3 { float x, y, z; };
V3    vec(float latDeg, float lonDeg)
{
    const float la = latDeg * kDeg, lo = lonDeg * kDeg;
    return { cosf(la) * cosf(lo), cosf(la) * sinf(lo), sinf(la) };
}
float dot(const V3 &a, const V3 &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float angle(const V3 &a, const V3 &b)
{
    const float d = dot(a, b);
    return acosf(d > 1 ? 1 : (d < -1 ? -1 : d));
}
// Point a fraction f of the way along the great circle a -> b (d = angle between).
V3 slerp(const V3 &a, const V3 &b, float d, float f)
{
    if (d < 1e-4f) return a;
    const float s = sinf(d), ka = sinf((1 - f) * d) / s, kb = sinf(f * d) / s;
    return { ka * a.x + kb * b.x, ka * a.y + kb * b.y, ka * a.z + kb * b.z };
}

Rgb mix(Rgb a, Rgb b, float k)
{
    k = k < 0 ? 0 : (k > 1 ? 1 : k);
    return { (uint8_t)(a.r + (b.r - a.r) * k), (uint8_t)(a.g + (b.g - a.g) * k), (uint8_t)(a.b + (b.b - a.b) * k) };
}
float ease(float u) { u = u < 0 ? 0 : (u > 1 ? 1 : u); return 1 - (1 - u) * (1 - u) * (1 - u); }

void thousands(long v, char *buf, size_t len)
{
    char raw[16];
    snprintf(raw, sizeof(raw), "%ld", v);
    const int n = strlen(raw);
    size_t o = 0;
    for (int i = 0; i < n && o + 1 < len; ++i)
    {
        if (i && (n - i) % 3 == 0 && o + 2 < len) buf[o++] = ',';
        buf[o++] = raw[i];
    }
    buf[o] = '\0';
}

// The view: centre, east and north unit vectors at the point facing us.
struct View
{
    V3 c, e, n;
    float zoom = 1;   // > 1: closer in, for short routes (the disc shows less of the Earth)
    void at(float latDeg, float lonDeg)
    {
        const float la = latDeg * kDeg, lo = lonDeg * kDeg;
        c = { cosf(la) * cosf(lo), cosf(la) * sinf(lo), sinf(la) };
        e = { -sinf(lo), cosf(lo), 0 };
        n = { -sinf(la) * cosf(lo), -sinf(la) * sinf(lo), cosf(la) };
    }
    // Panel position of a world point; false when it is round the back.
    bool project(const V3 &p, float &x, float &y) const
    {
        x = kCx + dot(p, e) * kR * zoom - 0.5f;   // inverse of the pixel-centre mapping below
        y = kCy - dot(p, n) * kR * zoom - 0.5f;
        const float dx = x + 0.5f - kCx, dy = y + 0.5f - kCy;
        return dot(p, c) > 0 && dx * dx + dy * dy <= (kR + 0.5f) * (kR + 0.5f);
    }
};
}

void Globe::subsolar(time_t t, float &lat, float &lon)
{
    // Same low-precision solar position as Sky::sunElevation.
    const double d   = t / 86400.0 - 10957.5;
    const double rad = M_PI / 180.0;
    const double g   = fmod(357.529 + 0.98560028 * d, 360.0) * rad;
    const double q   = fmod(280.459 + 0.98564736 * d, 360.0);
    const double L   = (q + 1.915 * sin(g) + 0.020 * sin(2 * g)) * rad;
    const double e   = (23.439 - 0.00000036 * d) * rad;
    const double ra  = atan2(cos(e) * sin(L), cos(L));
    const double dec = asin(sin(e) * sin(L));
    const double gmst = fmod(18.697374558 + 24.06570982441908 * d, 24.0) * 15.0 * rad;
    lat = (float)(dec / rad);
    double lo = fmod((ra - gmst) / rad + 540.0, 360.0) - 180.0;
    if (lo < -180) lo += 360;
    lon = (float)lo;
}

void Globe::render(FrameCanvas &c, const Route &r, uint32_t shownMs, time_t now)
{
    c.clear();
    const V3 a = vec(r.oLat, r.oLon), b = vec(r.dLat, r.dLon);
    const float d = angle(a, b);

    // How far along: the aircraft's place between the two ends.
    float frac = 0;
    if (r.landed) frac = 1;
    else if (!isnan(r.lat) && !isnan(r.lon))
    {
        const V3 p = vec(r.lat, r.lon);
        const float flown = angle(a, p), left = angle(p, b);
        frac = flown + left > 1e-4f ? flown / (flown + left) : 0;
    }

    // Face the middle of the route, turning in from the west as the screen opens.
    const V3 mid = slerp(a, b, d, 0.5f);
    const float midLat = asinf(mid.z) / kDeg, midLon = atan2f(mid.y, mid.x) / kDeg;
    View v;
    // Short routes closer in (London to Billund is a smudge on the whole
    // Earth): the route about 0.9 of the radius long, up to 4x (beyond that
    // the 1-degree land cells get blocky).
    const float chord = 2 * sinf(d / 2);
    v.zoom = chord > 1e-3f ? fminf(4.0f, fmaxf(1.0f, 0.9f / chord)) : 4.0f;
    v.at(midLat, midLon - 60.0f * (1 - ease(shownMs / 1600.0f)));

    // Sun: full light without a clock.
    const bool sunKnown = now > 1600000000;
    V3 sun{0, 0, 1};
    if (sunKnown)
    {
        float sl, so;
        subsolar(now, sl, so);
        sun = vec(sl, so);
    }

    // The globe, pixel by pixel: back-project to latitude and longitude.
    const Rgb oceanIn{16, 60, 120}, oceanOut{8, 34, 80};
    for (int py = kCy - kR; py <= kCy + kR; ++py)
        for (int px = kCx - kR; px <= kCx + kR; ++px)
        {
            const float sx = (px - kCx + 0.5f) / kR, sy = -(py - kCy + 0.5f) / kR;
            if (sx * sx + sy * sy > 1) continue;              // outside the disc
            const float x = sx / v.zoom, y = sy / v.zoom;     // on the unit sphere
            const float r2 = x * x + y * y;
            const float z = sqrtf(1 - r2);
            const V3 p{ z * v.c.x + x * v.e.x + y * v.n.x, z * v.c.y + x * v.e.y + y * v.n.y,
                        z * v.c.z + x * v.e.z + y * v.n.z };
            const float lat = asinf(p.z > 1 ? 1 : p.z) / kDeg, lon = atan2f(p.y, p.x) / kDeg;
            Rgb col;
            if (LandMask::isLand(lat, lon))
            {
                const float al = fabsf(lat);
                col = al > 66 ? Rgb{170, 180, 190} : mix(Rgb{52, 120, 62}, Rgb{150, 140, 90}, (al - 20) / 60);
            }
            else
                col = mix(oceanOut, oceanIn, z);
            float k = 0.55f + 0.45f * sqrtf(fmaxf(0, 1 - (sx * sx + sy * sy)));   // rounder: darker towards the edge
            if (sunKnown)
            {
                const float day = dot(p, sun);
                k *= day > 0.08f ? 1.0f : (day < -0.08f ? 0.16f : 0.16f + 0.84f * (day + 0.08f) / 0.16f);
            }
            c.set(px, py, FrameCanvas::scale(col, k));
        }

    // The route: drawn on after the globe has turned in. Solid (fading up to
    // the accent) where it has been, dashed ahead.
    const float drawn = ease((shownMs - 900.0f) / 1200.0f);
    constexpr int kSteps = 160;
    for (int i = 0; i <= kSteps; ++i)
    {
        const float f = (float)i / kSteps;
        if (f > drawn) break;
        float x, y;
        if (!v.project(slerp(a, b, d, f), x, y)) continue;
        if (f <= frac)
            c.set((int)lroundf(x), (int)lroundf(y), mix(FrameCanvas::scale(r.accent, 0.5f), r.accent, frac > 0 ? f / frac : 1));
        else if (i % 4 < 2)
            c.set((int)lroundf(x), (int)lroundf(y), Rgb{150, 160, 180});
    }
    // The two airports: small crosses, the destination brighter.
    for (int end = 0; end < 2; ++end)
    {
        float x, y;
        if (!v.project(end ? b : a, x, y)) continue;
        const Rgb col = end ? Rgb{255, 255, 255} : Rgb{200, 210, 230};
        const int ix = (int)lroundf(x), iy = (int)lroundf(y);
        c.set(ix, iy, col);
        const Rgb half = FrameCanvas::scale(col, 0.5f);
        c.set(ix + 1, iy, half); c.set(ix - 1, iy, half); c.set(ix, iy + 1, half); c.set(ix, iy - 1, half);
    }
    // The aircraft, pulsing, once the line reaches it.
    if (!r.landed && drawn >= frac)
    {
        float x, y;
        if (v.project(slerp(a, b, d, frac), x, y))
        {
            const int ix = (int)lroundf(x), iy = (int)lroundf(y);
            const float pulse = 0.6f + 0.4f * sinf(shownMs / 180.0f);
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    if (dx || dy)
                        c.set(ix + dx, iy + dy, mix(FrameCanvas::unpack(c.get(ix + dx, iy + dy)), r.accent, 0.8f * pulse));
            c.set(ix, iy, Rgb{255, 255, 255});
        }
    }

    // ── The column beside it ──────────────────────────────────────────────
    if (FrameCanvas::textWidth(r.ident, 2, 11) <= FrameCanvas::W - kTextX)
        c.text(kTextX, 2, r.ident, r.accent, 2, 11);
    else
        c.text(kTextX, 6, r.ident, r.accent, 1, 0, 10);
    c.text(kTextX, 19, r.from, Rgb{150, 200, 235}, 1, 0, 10);
    // A small chevron down to the destination.
    c.set(kTextX - 4, 30, Rgb{120, 130, 150}); c.set(kTextX - 3, 31, Rgb{120, 130, 150});
    c.set(kTextX - 4, 32, Rgb{120, 130, 150});
    c.text(kTextX, 28, r.to, Rgb{235, 240, 245}, 1, 0, 10);

    // Progress bar, filling as the route draws.
    constexpr int kBarW = 58;
    const int fill = (int)lroundf(kBarW * frac * drawn);
    for (int x = 0; x < kBarW; ++x)
        for (int y = 0; y < 3; ++y)
            c.set(kTextX + x, 40 + y, x < fill ? mix(FrameCanvas::scale(r.accent, 0.55f), r.accent, (float)x / kBarW)
                                               : Rgb{30, 34, 46});

    const float km = d * 6371.0f;
    char num[16], line[24];
    thousands(lroundf(r.landed ? km : km * frac), num, sizeof(num));
    snprintf(line, sizeof(line), "%s KM", num);
    c.text(kTextX, 46, line, Rgb{200, 205, 215});
    struct tm lt;
    time_t when = r.landed ? r.landedAt : r.eta;
    if (!when && !r.landed && sunKnown && r.speedKts > 60 && frac > 0)
        when = now + (time_t)(km * (1 - frac) / (r.speedKts * 1.852f) * 3600.0f);
    if (when > 1600000000)
    {
        localtime_r(&when, &lt);
        snprintf(line, sizeof(line), "%s %02d:%02d", r.landed ? "ARR" : "ETA", lt.tm_hour, lt.tm_min);
    }
    else if (r.landed)
        snprintf(line, sizeof(line), "LANDED");
    else
    {
        thousands(lroundf(km * (1 - frac)), num, sizeof(num));
        snprintf(line, sizeof(line), "%s LEFT", num);
    }
    c.text(kTextX, 55, line, Rgb{120, 130, 150}, 1, 0, 10);
}
