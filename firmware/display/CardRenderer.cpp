#include "display/CardRenderer.h"
#include "display/AircraftSprites.h"
#include <math.h>

namespace
{
    constexpr uint16_t kTransparent565 = 0xF81F;   // magenta in the logo files

    const Rgb kWhite     {240, 240, 240};
    const Rgb kTypeBlue  {150, 200, 255};
    const Rgb kAltGreen  {110, 220, 140};
    const Rgb kAmber     {255, 185,  60};
    const Rgb kCyan      { 90, 200, 210};
    const Rgb kLate      {255,  95,  80};
    const Rgb kGrey      {150, 155, 165};
    const Rgb kDimGrey   { 70,  75,  85};
    const Rgb kBarGrey   {200, 200, 205};
    const Rgb kEtaWhite  {235, 240, 245};
    const Rgb kDefaultAccent {90, 170, 255};

    // Left column: logo above the aircraft sprite.
    constexpr int kLeftX    = 2;
    constexpr int kLeftW    = 44;
    constexpr int kLogoX    = kLeftX + (kLeftW - 32) / 2;
    constexpr int kLogoY    = 2;
    constexpr int kSpriteY0 = 35, kSpriteY1 = 51;
    // Right column: text.
    constexpr int kTextX    = 48;
    constexpr int kRightX   = 125;                       // last usable column
    constexpr int kTextW    = kRightX + 1 - kTextX;      // 78 px = 13 chars
    // Bottom band.
    constexpr int kBandY    = 55;

    constexpr uint32_t kRouteShowMs = 5000;   // route, before switching to the airline name
    constexpr uint32_t kNameShowMs  = 3000;   // airline name
    constexpr uint32_t kFadeMs      = 350;    // cross-fade between the two

    constexpr uint32_t kGlintEveryMs = 7000;   // logo glint cadence
    constexpr uint32_t kGlintMs      = 650;    // glint sweep duration
    constexpr uint32_t kRabbitMs     = 900;    // approach-light run, marker -> threshold
    constexpr float    kMaxAltDeadReckonMs = 60000.0f;

    // 3x3 marker pointing left, drawn with a black outline so it reads over the strip.
    const char *const kMarker[3] = { "...#.", "#####", "...#." };
    // 5x3 climb / descent arrows.
    const char *const kArrowDown[3] = { "#####", ".###.", "..#.." };
    const char *const kArrowUp[3]   = { "..#..", ".###.", "#####" };

    Rgb blend(Rgb a, Rgb b, float k)
    {
        return { (uint8_t)(a.r + (b.r - a.r) * k), (uint8_t)(a.g + (b.g - a.g) * k),
                 (uint8_t)(a.b + (b.b - a.b) * k) };
    }

    void drawBits(FrameCanvas &c, const char *const *rows, int nRows, int x, int y, Rgb col)
    {
        for (int r = 0; r < nRows; ++r)
            for (int i = 0; rows[r][i]; ++i)
                if (rows[r][i] == '#') c.set(x + i, y + r, col);
    }

    // "12,345" into buf; no heap allocation (this runs 20 times a second).
    void withThousands(int v, char *buf, size_t len)
    {
        char digits[12];
        snprintf(digits, sizeof(digits), "%d", abs(v));
        size_t o = 0;
        const int n = (int)strlen(digits);
        if (v < 0 && o + 1 < len) buf[o++] = '-';
        for (int i = 0; i < n && o + 1 < len; ++i)
        {
            if (i && (n - i) % 3 == 0 && o + 1 < len) buf[o++] = ',';
            buf[o++] = digits[i];
        }
        buf[o] = '\0';
    }

    // Airline name short enough for one line: drop generic trailing words
    // ("BRITISH AIRWAYS SHUTTLE" -> "BRITISH"). Returns false if it still
    // doesn't fit, in which case the card just keeps the route.
    bool shortAirlineName(const char *name, int maxChars, char *out, size_t outLen)
    {
        static const char *const kGeneric[] = {
            "AIRWAYS", "AIRLINES", "AIRLINE", "AIR LINES", "INTERNATIONAL", "CONNECT",
            "SHUTTLE", "DOMESTIC", "EXPRESS", "AVIATION", "GROUP", "REGIONAL", "CARGO",
        };
        char buf[48];
        size_t n = 0;
        for (const char *p = name; *p && n + 1 < sizeof(buf); ++p)
            buf[n++] = (char)toupper((unsigned char)*p);
        buf[n] = '\0';
        while (n && buf[n - 1] == ' ') buf[--n] = '\0';

        bool trimmed = true;
        while (FrameCanvas::textWidth(buf) > maxChars * 6 - 1 && trimmed)
        {
            trimmed = false;
            for (const char *g : kGeneric)
            {
                const size_t gl = strlen(g);
                if (n > gl + 1 && strcmp(buf + n - gl, g) == 0 && buf[n - gl - 1] == ' ')
                {
                    n -= gl + 1;
                    buf[n] = '\0';
                    trimmed = true;
                    break;
                }
            }
        }
        if (n == 0 || FrameCanvas::textWidth(buf) > maxChars * 6 - 1) return false;
        snprintf(out, outLen, "%s", buf);
        return true;
    }

    // 0..1 brightness for a phase of length len at time t: fades in over the
    // first kFadeMs and out over the last kFadeMs.
    float phaseFade(uint32_t t, uint32_t len, uint32_t fadeMs)
    {
        if (t < fadeMs) return (float)t / fadeMs;
        if (t + fadeMs > len) return (float)(len - t) / fadeMs;
        return 1.0f;
    }

    const char *firstNonEmpty(const String &a, const String &b)
    {
        return a.length() ? a.c_str() : b.c_str();
    }

    void drawLogoTile(FrameCanvas &c, const FlightInfo &f, Rgb accent, uint32_t animMs)
    {
        const int x0 = kLogoX, y0 = kLogoY, n = AirlineLogo::WIDTH;
        if (f.airline_logo_rgb565.size() == (size_t)n * n)
        {
            // A diagonal glint sweeps across the logo every few seconds.
            const uint32_t phase = animMs % kGlintEveryMs;
            const int band = phase < kGlintMs ? (int)(phase * (2 * n + 8) / kGlintMs) - 4 : -100;
            for (int r = 0; r < n; ++r)
                for (int col = 0; col < n; ++col)
                {
                    uint16_t v = f.airline_logo_rgb565[r * n + col];
                    if (v == kTransparent565) continue;
                    const int d = abs(col + r - band);
                    if (d <= 2)
                        v = FrameCanvas::pack(blend(FrameCanvas::unpack(v), kWhite, d == 0 ? 0.55f : 0.25f));
                    c.set(x0 + col, y0 + r, v);
                }
            return;
        }
        // No logo: accent-bordered tile with the operator code.
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
            {
                const bool edge = x == 0 || y == 0 || x == n - 1 || y == n - 1;
                c.set(x0 + x, y0 + y, edge ? accent : FrameCanvas::scale(accent, 0.28f));
            }
        const char *code = f.operator_iata.length() ? f.operator_iata.c_str()
                         : f.operator_icao.length() ? f.operator_icao.c_str()
                         : f.ident.c_str();
        const int maxChars = f.operator_iata.length() || f.operator_icao.length() ? 4 : 3;
        c.text(x0 + (n - FrameCanvas::textWidth(code, 1, 0, maxChars)) / 2, y0 + 12, code, kWhite, 1, 0, maxChars);
    }

    void drawSprite(FrameCanvas &c, const FlightInfo &f, Rgb accent, bool faceRight)
    {
        bool known = false;
        const AircraftSprites::Kind kind = AircraftSprites::classify(f.aircraft_code, known);
        const AircraftSprites::Sprite &sp = AircraftSprites::get(kind);
        const int x = kLeftX + (kLeftW - sp.w) / 2;
        const int y = kSpriteY0 + (kSpriteY1 - kSpriteY0 + 1 - sp.h) / 2;
        AircraftSprites::draw(c, sp, x, y, accent, faceRight, !known);
    }

    void drawStrip(FrameCanvas &c, const ApproachStatus &s, Rgb accent, uint32_t animMs, bool landed)
    {
        // Runway threshold "piano keys", then the approach strip.
        // right = 121: the marker (mx-1..mx+3) and its 1px black halo stay at
        // least 1px clear of the border at x 127.
        const int barsX = 52, left = 64, right = 121, y = kBandY + 3;
        for (int x = barsX; x <= barsX + 8; x += 2)
            for (int yy = kBandY + 1; yy <= kBandY + 5; ++yy)
                c.set(x, yy, kBarGrey);

        if (isnan(s.progress) && !landed)
        {
            for (int x = left; x <= right; ++x)
                if (x % 3 == 0) c.set(x, y, kDimGrey);
            return;
        }
        const float progress = landed ? 1.0f : s.progress;
        const int mx = (int)lroundf(right - progress * (right - left - 2));
        for (int x = left; x <= right; ++x)
        {
            if (x >= mx)           c.set(x, y, FrameCanvas::scale(accent, 0.45f));
            else if (x % 3 == 0)   c.set(x, y, kDimGrey);
        }

        // Approach lights: a "rabbit" runs from the aircraft to the threshold.
        if (!landed && s.phase == ApproachStatus::Approach && mx - 2 > left)
        {
            const float q   = (float)(animMs % kRabbitMs) / kRabbitMs;
            const int   pos = (int)lroundf((mx - 2) - q * ((mx - 2) - left));
            c.set(pos, y, Rgb{255, 255, 255});
            c.set(pos + 1, y, Rgb{255, 200, 110});
            c.set(pos + 2, y, Rgb{140, 92, 25});
        }

        // Marker: clear a 1px halo, then draw.
        const int ox = mx - 1, oy = y - 1;
        for (int r = -1; r <= 3; ++r)
            for (int col = -1; col <= 5; ++col)
                c.set(ox + col, oy + r, (uint16_t)0);
        drawBits(c, kMarker, 3, ox, oy, Rgb{255, 255, 255});
    }

    void drawBorder(FrameCanvas &c, Rgb col)
    {
        for (int x = 0; x < FrameCanvas::W; ++x) { c.set(x, 0, col); c.set(x, FrameCanvas::H - 1, col); }
        for (int y = 0; y < FrameCanvas::H; ++y) { c.set(0, y, col); c.set(FrameCanvas::W - 1, y, col); }
    }
}

Rgb CardRenderer::accentFor(const FlightInfo &f)
{
    const auto &px = f.airline_logo_rgb565;
    if (px.empty()) return kDefaultAccent;

    // Bucket saturated, reasonably bright pixels by hue; average the busiest bucket.
    constexpr int kBuckets = 12;
    uint32_t cnt[kBuckets] = {}, sr[kBuckets] = {}, sg[kBuckets] = {}, sb[kBuckets] = {};
    for (uint16_t v : px)
    {
        if (v == kTransparent565) continue;
        const Rgb c = FrameCanvas::unpack(v);
        const int mx = max(c.r, max(c.g, c.b)), mn = min(c.r, min(c.g, c.b));
        if (mx < 70 || (mx - mn) < mx * 0.35f) continue;
        float h;
        const float d = mx - mn;
        if (mx == c.r)      h = fmodf((c.g - c.b) / d, 6.0f);
        else if (mx == c.g) h = (c.b - c.r) / d + 2.0f;
        else                h = (c.r - c.g) / d + 4.0f;
        if (h < 0) h += 6.0f;
        const int b = (int)(h * kBuckets / 6.0f) % kBuckets;
        ++cnt[b]; sr[b] += c.r; sg[b] += c.g; sb[b] += c.b;
    }
    int best = -1;
    for (int b = 0; b < kBuckets; ++b)
        if (cnt[b] > 20 && (best < 0 || cnt[b] > cnt[best])) best = b;
    if (best < 0) return kDefaultAccent;

    float r = (float)sr[best] / cnt[best], g = (float)sg[best] / cnt[best], b = (float)sb[best] / cnt[best];
    // Normalise so the strongest channel is full brightness — reads better on LEDs.
    const float m = max(r, max(g, b));
    const float k = 255.0f / m;
    r *= k; g *= k; b *= k;
    // Very dark hues (navy, deep blue) are dim on LEDs: lift towards white
    // until perceived brightness is readable, keeping the hue.
    constexpr float kMinLuma = 120.0f;
    const float luma = 0.30f * r + 0.59f * g + 0.11f * b;
    if (luma < kMinLuma)
    {
        const float t = (kMinLuma - luma) / (255.0f - luma);
        r += (255 - r) * t; g += (255 - g) * t; b += (255 - b) * t;
    }
    return { (uint8_t)r, (uint8_t)g, (uint8_t)b };
}

void CardRenderer::render(FrameCanvas &c, const FlightInfo &f, const ApproachStatus &s,
                          Rgb accent, const Options &opt)
{
    c.clear();
    if (opt.border) drawBorder(c, accent);

    // ── Left column: logo and aircraft ───────────────────────────────────────
    drawLogoTile(c, f, accent, opt.animMs);
    drawSprite(c, f, accent, opt.spriteRight);

    // ── Right column ─────────────────────────────────────────────────────────
    // All text goes through fixed buffers: drawing a frame allocates nothing.
    constexpr int kCols = kTextW / 6;   // 13 characters

    // Callsign / flight number, 2x font with a tighter 11px pitch.
    const char *ident = firstNonEmpty(f.ident_iata, f.ident);
    if (FrameCanvas::textWidth(ident, 2, 11) <= kTextW)
        c.text(kTextX, 2, ident, accent, 2, 11);
    else
        c.text(kTextX, 6, ident, accent, 1, 0, kCols);

    // Route line, cross-fading with the shortened airline name on the same
    // line (accent colour). The type row below is never touched.
    const char *org = firstNonEmpty(f.origin.code_iata, f.origin.code_icao);
    const char *dst = firstNonEmpty(f.destination.code_iata, f.destination.code_icao);
    const bool hasRoute = org[0] && dst[0];
    char route[24] = "", airline[kCols + 1] = "";
    if (hasRoute) snprintf(route, sizeof(route), "%s > %s", org, dst);
    const bool hasAirline = shortAirlineName(f.airline_display_name_full.c_str(), kCols, airline, sizeof(airline));

    if (hasRoute && hasAirline)
    {
        const uint32_t t = opt.animMs % (kRouteShowMs + kNameShowMs);
        if (t < kRouteShowMs)
            c.text(kTextX, 18, route, FrameCanvas::scale(kWhite, phaseFade(t, kRouteShowMs, kFadeMs)), 1, 0, kCols);
        else
            c.text(kTextX, 18, airline, FrameCanvas::scale(accent, phaseFade(t - kRouteShowMs, kNameShowMs, kFadeMs)));
    }
    else if (hasRoute)
        c.text(kTextX, 18, route, kWhite, 1, 0, kCols);
    else if (hasAirline)
        c.text(kTextX, 18, airline, accent);

    c.text(kTextX, 27, firstNonEmpty(f.aircraft_display_name_short, f.aircraft_code), kTypeBlue, 1, 0, kCols);

    // Altitude + speed (or, once landed, the touchdown time), with a
    // climb/descent arrow when there is room. The altitude comes in already
    // smoothed (opt.altFt); without it, dead-reckon from the vertical rate.
    char alt[16] = "", spd[12] = "";
    if (opt.landed)
    {
        if (opt.landedAt)
        {
            struct tm lt;
            localtime_r(&opt.landedAt, &lt);
            snprintf(alt, sizeof(alt), "LANDED %02d:%02d", lt.tm_hour, lt.tm_min);
        }
        c.text(kTextX, 36, alt[0] ? alt : "LANDED", kAmber);
    }
    else
    {
        double a = opt.altFt;
        if (isnan(a) && !isnan(f.baro_altitude))
        {
            a = f.baro_altitude;
            if (!isnan(f.vertical_rate))
                a += f.vertical_rate * min((float)opt.dataAgeMs, kMaxAltDeadReckonMs) / 60000.0;
        }
        if (!isnan(a))
        {
            if (a < 0) a = 0;
            char num[12];
            withThousands((int)lround(a / 5.0) * 5, num, sizeof(num));   // 5 ft steps: reads as a smooth count
            snprintf(alt, sizeof(alt), "%sFT", num);
        }
        if (!isnan(f.velocity))
            snprintf(spd, sizeof(spd), "%dKT", (int)lround(f.velocity));
        const bool arrow = alt[0] && !isnan(f.vertical_rate) && fabs(f.vertical_rate) >= 200;
        int x = kTextX;
        c.text(x, 36, alt, kAltGreen);
        x += FrameCanvas::textWidth(alt);
        const int spdW = FrameCanvas::textWidth(spd);
        if (arrow && x + 2 + 5 + 3 + spdW <= kRightX + 1)
        {
            drawBits(c, f.vertical_rate < 0 ? kArrowDown : kArrowUp, 3, x + 2, 38, kAltGreen);
            x += 2 + 5;
        }
        if (spd[0] && x + (alt[0] ? 4 : 0) + spdW <= kRightX + 1)
            c.text(x + (alt[0] ? 4 : 0), 36, spd, kAltGreen);
    }

    // Status line. Once landed: early/late against the schedule when known
    // (AeroAPI), otherwise the runway.
    char label[24];
    ApproachModel::label(s, label, sizeof(label));
    Rgb statusCol = kGrey;
    switch (s.phase)
    {
        case ApproachStatus::Approach:
        case ApproachStatus::Landing:
            statusCol = FrameCanvas::scale(kAmber, 0.55f + 0.45f * sinf(opt.animMs / 230.0f));
            break;
        case ApproachStatus::Inbound:  statusCol = kAmber; break;
        case ApproachStatus::Departed:
        case ApproachStatus::Outbound: statusCol = kCyan;  break;
        default: break;
    }
    if (opt.goAround)
    {
        snprintf(label, sizeof(label), s.runway[0] ? "GO AROUND %s" : "GO AROUND", s.runway);
        statusCol = (opt.animMs / 300) % 2 ? kLate : Rgb{255, 255, 255};
    }
    else if (opt.landed)
    {
        if (opt.landedAt && f.scheduled_on)
        {
            const long mins = lround((double)(opt.landedAt - f.scheduled_on) / 60.0);
            if (mins <= -1)     { snprintf(label, sizeof(label), "%ld MIN EARLY", -mins); statusCol = kAltGreen; }
            else if (mins >= 1) { snprintf(label, sizeof(label), "%ld MIN LATE", mins);   statusCol = kLate; }
            else                { snprintf(label, sizeof(label), "ON TIME");               statusCol = kAltGreen; }
        }
        else
        {
            snprintf(label, sizeof(label), s.runway[0] ? "RUNWAY %s" : "HEATHROW", s.runway);
            statusCol = kCyan;
        }
    }
    c.text(kTextX, 45, label, statusCol, 1, 0, kCols);

    // ── Bottom band: ETA / distance, then the approach strip ─────────────────
    char eta[16] = "";
    if (opt.landed)
        ;
    else if (!isnan(s.etaSec))
    {
        const int secs = (int)lroundf(s.etaSec);
        snprintf(eta, sizeof(eta), "ETA %d:%02d", secs / 60, secs % 60);
    }
    else if (!isnan(s.distKm))
    {
        if (s.distKm < 10) snprintf(eta, sizeof(eta), "%.1fKM", s.distKm);
        else               snprintf(eta, sizeof(eta), "%dKM", (int)lroundf(s.distKm));
    }
    c.text(kLeftX, kBandY, eta, kEtaWhite);

    drawStrip(c, s, accent, opt.animMs, opt.landed);
}
