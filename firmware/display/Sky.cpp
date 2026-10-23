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
        {-18, {  2,   4,  18}, { 58,  38,  44}},   // night: deep navy over a city's warm skyglow
        { -9, {  8,  12,  44}, { 60,  42,  86}},   // nautical twilight
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
        l.windDir = m.windDir;
        l.windKt  = m.windKt;
        l.gustKt  = m.gustKt;
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

Sky::Look Sky::atPlace(const Metar &m, time_t t, double lat, double lon)
{
    Look l;
    if (t < 1600000000) return l;
    l.known = true;
    l.sunElev = sunElevation(lat, lon, t);
    readWeather(m, l);
    l.frost = (m.valid && m.tempC != -99 && m.tempC <= 0) || l.precip == Snow;
    finish(l);
    return l;
}

Sky::Look Sky::at(const Metar &m, time_t t)
{
    Look l;
    if (t < 1600000000) return l;   // clock not synced
    l.known = true;
    l.sunElev = sunElevation(g_airport.lat, g_airport.lon, t);
    readWeather(m, l);
    l.frost = (m.valid && m.tempC != -99 && m.tempC <= 0) || l.precip == Snow;
    struct tm lt;
    localtime_r(&t, &lt);
    // Fireworks on Bonfire Night and New Year's Eve evenings (into the small hours of the 1st).
    if ((lt.tm_mon == 10 && lt.tm_mday == 5 && lt.tm_hour >= 17) ||
        (lt.tm_mon == 11 && lt.tm_mday == 31 && lt.tm_hour >= 17) ||
        (lt.tm_mon == 0 && lt.tm_mday == 1 && lt.tm_hour < 2))
        l.season = Look::Fireworks;
    else if (lt.tm_mon == 11 && lt.tm_mday <= 26)
        l.season = Look::Christmas;
    else if (lt.tm_mon == 9 && lt.tm_mday >= 18)   // the run-up to Halloween, to the 31st
        l.season = Look::Halloween;
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
    if (strstr(name, "snow"))  { l.precip = Snow;    l.cloud = 3; l.frost = true; }
    if (strstr(name, "frost"))     l.frost = true;
    if (strstr(name, "windy"))     { l.windDir = 220; l.windKt = 22; l.gustKt = 34; }   // a gusty south-westerly
    if (strstr(name, "storm"))     { l.windDir = 250; l.windKt = 26; l.gustKt = 41; }
    if (strstr(name, "christmas")) l.season = Look::Christmas;
    if (strstr(name, "fireworks")) l.season = Look::Fireworks;
    if (strstr(name, "halloween")) l.season = Look::Halloween;
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
    // Ordered (Bayer) dithering: the panel only has a few levels in a dark
    // sky, and without it the gradient shows as two or three flat bands.
    static const uint8_t kBayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    for (int y = 0; y < horizonY; ++y)
    {
        const float k = (float)y / (horizonY - 1);
        Rgb col = mix(l.top, l.horizon, k * k);   // colour gathers at the horizon
        if (flash > 0) col = mix(col, Rgb{170, 170, 210}, flash);
        for (int x = 0; x < FrameCanvas::W; ++x)
        {
            const int t = kBayer[y & 3][x & 3];   // 0..15
            c.set(x, y, Rgb{(uint8_t)min(255, col.r + t / 2), (uint8_t)min(255, col.g + t / 4),
                            (uint8_t)min(255, col.b + t / 2)});
        }
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
    if ((1 - ct) / 2 < 0.06f) return;   // under 6% lit: nothing to see
    const int ir = (int)ceilf(r);
    for (int dy = -ir; dy <= ir; ++dy)
        for (int dx = -ir; dx <= ir; ++dx)
        {
            const float d = sqrtf(dx * dx + dy * dy);
            if (d > r + 0.3f) continue;
            const float yn = dy / r, xn = dx / r;
            const float edge = sqrtf(max(0.0f, 1 - yn * yn));
            const bool on = phase < 0.5f ? xn > ct * edge : xn < -ct * edge;
            blend(c, cx + dx, cy + dy, on ? lit : FrameCanvas::scale(lit, 0.05f), k * clamp01(r + 0.5f - d));
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
    // Readable against the sky on the LEDs: by day a clear step darker and
    // bluer than the horizon; against a sunset a dusky violet (a dark grey
    // there crushes to black and the skyline vanishes into the glow); the
    // night colour after dark. Scenery darker or lighter than the main
    // silhouette (hills, roofs) keeps that difference.
    const float rel = (nightColour.r + nightColour.g + nightColour.b) / 147.0f;   // 1: the silhouette
    auto mul = [](Rgb c, float k) {
        return Rgb{(uint8_t)min(255.0f, c.r * k), (uint8_t)min(255.0f, c.g * k), (uint8_t)min(255.0f, c.b * k)};
    };
    const Rgb day  = mul(mix(l.horizon, Rgb{40, 60, 100}, 0.35f), 0.55f * rel);
    const Rgb dusk = mul(Rgb{58, 34, 72}, rel);
    // After dark: darker than the city's skyglow behind them.
    return mix(mix(day, dusk, l.golden), FrameCanvas::scale(nightColour, 0.55f), l.night);
}

Rgb Sky::rim(const Look &l, float &strength)
{
    // Light catching the tops of the buildings: the low sun at sunset
    // (warm), twilight (cool blue), and a faint moonlit edge at night so the
    // rooftops still read against the dark upper sky. None by day.
    strength = 0;
    if (!l.known) return Rgb{0, 0, 0};
    const float dusk = clamp01(l.golden + (l.night > 0.2f && l.night < 0.85f ? 0.6f : 0.0f));
    strength = max(0.65f * dusk, l.night >= 0.85f ? 0.4f : 0.0f);
    return mix(Rgb{255, 170, 110}, Rgb{110, 125, 175}, clamp01((l.night - 0.2f) / 0.5f));
}

float Sky::mapLight(const Look &l)
{
    return l.known ? 1.0f - 0.45f * l.night : 1.0f;
}

// The next time the sun's centre crosses -0.83 degrees (refraction and its
// radius: the moment the top edge meets the horizon), searched in 10-minute
// steps and refined to the minute; worked out again only when the cached
// event has passed or the airport has changed.
time_t Sky::nextSunEvent(time_t t, bool &isSunrise)
{
    static time_t s_at = 0, s_from = 0;
    static bool s_rise = false;
    static double s_lat = 0, s_lon = 0;
    if (t < 1600000000) return 0;
    if (s_at && t < s_at && t >= s_from && s_lat == g_airport.lat && s_lon == g_airport.lon)
    {
        isSunrise = s_rise;
        return s_at;
    }
    constexpr float kHorizon = -0.83f;
    s_from = t; s_lat = g_airport.lat; s_lon = g_airport.lon; s_at = 0;
    float prev = sunElevation(s_lat, s_lon, t) - kHorizon;
    for (time_t u = t + 600; u <= t + 86400 + 600; u += 600)
    {
        const float now = sunElevation(s_lat, s_lon, u) - kHorizon;
        if ((prev < 0) != (now < 0))
        {
            time_t lo = u - 600, hi = u;   // refine to the minute
            while (hi - lo > 60)
            {
                const time_t mid = lo + (hi - lo) / 2;
                if ((sunElevation(s_lat, s_lon, mid) - kHorizon < 0) == (prev < 0)) lo = mid; else hi = mid;
            }
            s_at = hi;
            s_rise = now >= 0;
            break;
        }
        prev = now;
    }
    isSunrise = s_rise;
    return s_at;
}
