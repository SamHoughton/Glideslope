#include "display/Sky.h"
#include "config/Airport.h"
#include <math.h>

namespace
{
    float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

    Rgb mix(Rgb a, Rgb b, float k)
    {
        k = clamp01(k);
        return { (uint8_t)(a.r + (b.r - a.r) * k), (uint8_t)(a.g + (b.g - a.g) * k), (uint8_t)(a.b + (b.b - a.b) * k) };
    }

    // Sky colours by sun elevation: {elevation, top, horizon}.
    struct Key { float elev; Rgb top, horizon; };
    const Key kKeys[] = {
        {-18, {  0,   0,   4}, {  5,   8,  22}},   // night
        { -9, {  6,  10,  36}, { 30,  30,  78}},   // nautical twilight
        { -3, { 18,  24,  72}, {150,  70,  80}},   // civil twilight: pink horizon
        {  2, { 36,  56, 118}, {235, 125,  50}},   // sun on the horizon: orange
        {  8, { 22,  60, 128}, {110, 140, 170}},   // morning / evening
        { 20, { 18,  56, 130}, { 80, 130, 178}},   // day
    };
    constexpr int kKeyCount = sizeof(kKeys) / sizeof(kKeys[0]);

    void skyColours(float elev, Rgb &top, Rgb &horizon)
    {
        if (elev <= kKeys[0].elev) { top = kKeys[0].top; horizon = kKeys[0].horizon; return; }
        for (int i = 1; i < kKeyCount; ++i)
            if (elev <= kKeys[i].elev)
            {
                const float k = (elev - kKeys[i - 1].elev) / (kKeys[i].elev - kKeys[i - 1].elev);
                top = mix(kKeys[i - 1].top, kKeys[i].top, k);
                horizon = mix(kKeys[i - 1].horizon, kKeys[i].horizon, k);
                return;
            }
        top = kKeys[kKeyCount - 1].top;
        horizon = kKeys[kKeyCount - 1].horizon;
    }

    // Cloud and weather from the METAR.
    void readWeather(const Metar &m, Sky::Look &l)
    {
        if (!m.valid) return;
        const char *raw = m.raw;
        if (strstr(raw, " OVC") || strstr(raw, " VV")) l.cloud = 3;
        else if (strstr(raw, " BKN"))                  l.cloud = 2;
        else if (strstr(raw, " SCT") || strstr(raw, " FEW")) l.cloud = 1;
        if (strstr(m.wx, "TS"))                         l.precip = Sky::Thunder;
        else if (strstr(m.wx, "SN") || strstr(m.wx, "SG")) l.precip = Sky::Snow;
        else if (strstr(m.wx, "RA") || strstr(m.wx, "DZ") || strstr(m.wx, "SH")) l.precip = Sky::Rain;
        l.fog = strstr(m.wx, "FG") || (m.visM >= 0 && m.visM < 1500);
    }

    void finish(Sky::Look &l)
    {
        l.night  = clamp01((2.0f - l.sunElev) / 14.0f);
        l.golden = clamp01(1.0f - fabsf(l.sunElev - 1.0f) / 6.0f);
        if (l.cloud == 3) l.golden *= 0.3f;
        l.lightsOn = l.sunElev < 1.0f || l.fog || (l.cloud == 3 && l.precip != Sky::NoPrecip);
        skyColours(l.sunElev, l.top, l.horizon);
        // Cloud greys the sky; overcast mostly hides the colour.
        if (l.cloud >= 2)
        {
            const float k = l.cloud == 3 ? 0.8f : 0.35f;
            const Rgb greyTop = FrameCanvas::scale(Rgb{95, 100, 112}, 1.0f - 0.85f * l.night);
            const Rgb greyHor = FrameCanvas::scale(Rgb{130, 134, 142}, 1.0f - 0.85f * l.night);
            l.top = mix(l.top, greyTop, k);
            l.horizon = mix(l.horizon, greyHor, k);
        }
        if (l.precip != Sky::NoPrecip)
        {
            l.top = FrameCanvas::scale(l.top, 0.75f);
            l.horizon = FrameCanvas::scale(l.horizon, 0.8f);
        }
    }

    // Pseudo-random but fixed per index.
    uint32_t hash(uint32_t x)
    {
        x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16;
        return x;
    }

    void blend(FrameCanvas &c, int x, int y, Rgb col, float k)
    {
        if ((unsigned)x >= (unsigned)FrameCanvas::W || (unsigned)y >= (unsigned)FrameCanvas::H) return;
        c.set(x, y, mix(FrameCanvas::unpack(c.get(x, y)), col, k));
    }

    void disc(FrameCanvas &c, int cx, int cy, float r, Rgb col, float k = 1)
    {
        const int ir = (int)ceilf(r);
        for (int dy = -ir; dy <= ir; ++dy)
            for (int dx = -ir; dx <= ir; ++dx)
            {
                const float d = sqrtf(dx * dx + dy * dy);
                if (d <= r) blend(c, cx + dx, cy + dy, col, k * clamp01(r + 0.5f - d));
            }
    }
}

float Sky::sunElevation(double lat, double lon, time_t t)
{
    // Low-precision solar position (good to a fraction of a degree).
    const double d   = t / 86400.0 - 10957.5;                     // days since J2000
    const double rad = M_PI / 180.0;
    const double g   = fmod(357.529 + 0.98560028 * d, 360.0) * rad;
    const double q   = fmod(280.459 + 0.98564736 * d, 360.0);
    const double L   = (q + 1.915 * sin(g) + 0.020 * sin(2 * g)) * rad;
    const double e   = (23.439 - 0.00000036 * d) * rad;
    const double ra  = atan2(cos(e) * sin(L), cos(L));
    const double dec = asin(sin(e) * sin(L));
    const double gmst = fmod(18.697374558 + 24.06570982441908 * d, 24.0) * 15.0 * rad;
    const double h   = gmst + lon * rad - ra;
    const double el  = asin(sin(lat * rad) * sin(dec) + cos(lat * rad) * cos(dec) * cos(h));
    return (float)(el / rad);
}

Sky::Look Sky::at(const Metar &m, time_t t)
{
    Look l;
    if (t < 1600000000) return l;   // clock not synced
    l.known = true;
    l.sunElev = sunElevation(g_airport.lat, g_airport.lon, t);
    readWeather(m, l);
    finish(l);
    return l;
}

Sky::Look Sky::preview(const char *name)
{
    Look l;
    l.known = true;
    l.sunElev = strstr(name, "night") ? -25 : strstr(name, "twilight") ? -5 : strstr(name, "golden") ? 1.5f : 30;
    if (strstr(name, "overcast")) l.cloud = 3;
    else if (strstr(name, "cloud")) l.cloud = 2;
    else l.cloud = 1;
    if (strstr(name, "rain"))  { l.precip = Rain;    l.cloud = max<uint8_t>(l.cloud, 2); }
    if (strstr(name, "snow"))  { l.precip = Snow;    l.cloud = 3; }
    if (strstr(name, "storm")) { l.precip = Thunder; l.cloud = 3; }
    if (strstr(name, "fog"))   l.fog = true;
    finish(l);
    return l;
}

void Sky::drawSky(FrameCanvas &c, const Look &l, uint32_t tMs, int horizonY)
{
    if (!l.known) return;
    // Thunder: the whole sky lights up now and then.
    float flash = 0;
    if (l.precip == Thunder)
    {
        const uint32_t ph = tMs % 2900;
        if (ph < 70 || (ph > 140 && ph < 190)) flash = 0.55f;
    }
    for (int y = 0; y < horizonY; ++y)
    {
        const float k = (float)y / (horizonY - 1);
        Rgb col = mix(l.top, l.horizon, k * k);   // colour gathers at the horizon
        if (flash > 0) col = mix(col, Rgb{170, 170, 210}, flash);
        for (int x = 0; x < FrameCanvas::W; ++x) c.set(x, y, col);
    }

    // Stars, twinkling, fading out through twilight and behind cloud.
    const float stars = clamp01((-l.sunElev - 6.0f) / 6.0f) * (l.cloud == 3 ? 0.0f : l.cloud == 2 ? 0.35f : 1.0f);
    if (stars > 0.02f && flash == 0)
        for (int i = 0; i < 34; ++i)
        {
            const uint32_t h = hash(i * 7919 + 17);
            const int x = h % FrameCanvas::W, y = (h >> 8) % (horizonY * 2 / 3);
            const float tw = 0.55f + 0.45f * sinf(tMs / (300.0f + (h >> 20) % 400) + i);
            blend(c, x, y, Rgb{220, 225, 255}, stars * tw * (0.5f + ((h >> 16) % 50) / 100.0f));
        }

    // The moon by night, the sun by day (low and large in golden hour).
    if (l.cloud < 3)
    {
        if (l.sunElev < -6)
        {
            const float k = clamp01((-l.sunElev - 6.0f) / 6.0f) * (l.cloud == 2 ? 0.5f : 1.0f);
            drawMoon(c, 22, 9, 2.8f, moonPhase(time(nullptr)), Rgb{215, 215, 190}, k);
        }
        else if (l.sunElev > -2)
        {
            const float rise = clamp01(l.sunElev / 25.0f);
            const int sy = (int)lroundf(horizonY - 3 - rise * (horizonY - 14));
            const Rgb sun = mix(Rgb{255, 150, 60}, Rgb{255, 240, 200}, clamp01(l.sunElev / 10.0f));
            const float r = 2.5f + 1.5f * l.golden;
            disc(c, 24, sy, r + 3, sun, 0.18f * (l.cloud == 2 ? 0.5f : 1.0f));   // glow
            disc(c, 24, sy, r, sun, l.cloud == 2 ? 0.6f : 1.0f);
        }
    }

    // Clouds drifting slowly across.
    if (l.cloud > 0)
    {
        const Rgb day{175, 180, 190}, dusk{200, 120, 105}, night{24, 28, 40};
        Rgb body = mix(mix(day, dusk, l.golden), night, l.night);
        if (flash > 0) body = mix(body, Rgb{200, 200, 230}, flash);
        const int count = l.cloud == 1 ? 2 : l.cloud == 2 ? 4 : 7;
        const float alpha = l.cloud == 3 ? 0.75f : 0.6f;
        for (int i = 0; i < count; ++i)
        {
            const uint32_t h = hash(i * 104729 + 3);
            const int span = FrameCanvas::W + 40;
            const int x0 = (int)((h % span + tMs / (90 + (h >> 12) % 60)) % span) - 20;
            const int y0 = 4 + (h >> 8) % (l.cloud == 3 ? 10 : 16);
            const int w = 10 + (h >> 16) % 14;
            for (int j = 0; j < 3; ++j)
                disc(c, x0 + j * w / 3, y0 + (j == 1 ? -2 : 0), 2.5f + (j == 1 ? 1.5f : 0.5f), body, alpha);
        }
    }
}

float Sky::moonPhase(time_t t)
{
    // Days since the new moon of 6 January 2000, 18:14 UTC, in synodic months.
    const double days = t / 86400.0 + 2440587.5 - 2451550.26;
    const double p = fmod(days / 29.530588853, 1.0);
    return (float)(p < 0 ? p + 1 : p);
}

void Sky::drawMoon(FrameCanvas &c, int cx, int cy, float r, float phase, Rgb lit, float k)
{
    // Lit where a pixel lies on the sunlit side of the terminator (an
    // ellipse whose width follows the phase); waxing lights the right.
    const float ct = cosf(phase * 2 * (float)M_PI);
    const int ir = (int)ceilf(r);
    for (int dy = -ir; dy <= ir; ++dy)
        for (int dx = -ir; dx <= ir; ++dx)
        {
            const float d = sqrtf(dx * dx + dy * dy);
            if (d > r + 0.3f) continue;
            const float yn = dy / r, xn = dx / r;
            const float edge = sqrtf(max(0.0f, 1 - yn * yn));
            const bool on = phase < 0.5f ? xn > ct * edge : xn < -ct * edge;
            blend(c, cx + dx, cy + dy, on ? lit : FrameCanvas::scale(lit, 0.12f), k * clamp01(r + 0.5f - d));
        }
}

void Sky::drawWeather(FrameCanvas &c, const Look &l, uint32_t tMs, int x0, int x1)
{
    if (!l.known) return;
    if (l.precip == Rain || l.precip == Thunder)
    {
        const Rgb drop = mix(Rgb{165, 175, 195}, Rgb{120, 140, 185}, l.night);
        for (int i = 0; i < 46; ++i)
        {
            const uint32_t h = hash(i * 2654435761u);
            const int y = (int)((h % 80 + tMs / 9) % 80) - 8;
            const int x = (int)((h >> 8) % (FrameCanvas::W + 20)) - y / 3;
            for (int s = 0; s < 3; ++s) blend(c, x - s / 2, y - s, drop, 0.75f - s * 0.15f);
        }
    }
    else if (l.precip == Snow)
    {
        const Rgb flake = mix(Rgb{235, 238, 245}, Rgb{150, 155, 170}, l.night);
        for (int i = 0; i < 40; ++i)
        {
            const uint32_t h = hash(i * 40503 + 9);
            const int y = (int)((h % 72 + tMs / (45 + h % 30)) % 72) - 4;
            const int x = (int)((h >> 8) % FrameCanvas::W + 2.0f * sinf(tMs / 500.0f + i));
            blend(c, x, y, flake, 0.85f);
        }
    }
    if (l.fog)
    {
        // Thicker towards the ground.
        const Rgb haze = mix(Rgb{125, 130, 138}, Rgb{30, 32, 40}, l.night);
        uint16_t *p = c.pixels();
        for (int y = 0; y < FrameCanvas::H; ++y)
        {
            const float k = 0.25f + 0.35f * y / FrameCanvas::H;
            for (int x = max(0, x0); x <= min(FrameCanvas::W - 1, x1); ++x)
            {
                uint16_t &v = p[y * FrameCanvas::W + x];
                v = FrameCanvas::pack(mix(FrameCanvas::unpack(v), haze, k));
            }
        }
    }
}

Rgb Sky::shade(const Look &l, Rgb nightColour)
{
    if (!l.known) return nightColour;
    // By day buildings are a hazy grey-blue a little darker than the sky
    // near the horizon; in golden hour and at night, dark silhouettes.
    const Rgb day = mix(nightColour, FrameCanvas::scale(l.horizon, 0.6f), 0.6f);
    return mix(day, nightColour, max(l.night, l.golden));
}

float Sky::mapLight(const Look &l)
{
    return l.known ? 1.0f - 0.45f * l.night : 1.0f;
}
