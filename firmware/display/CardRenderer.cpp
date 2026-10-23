#include "display/CardRenderer.h"
#include "display/AircraftSprites.h"
#include "display/Fonts.h"
#include "config/Airport.h"
#include "config/Airports.h"
#include "config/RuntimeConfig.h"
#include <math.h>

namespace
{
    constexpr uint16_t kTransparent565 = 0xF81F;   // magenta in the logo files

    const Rgb kWhite     {240, 240, 240};
    const Rgb kInk       {235, 238, 245};
    const Rgb kSoft      {150, 160, 180};
    const Rgb kFaint     { 80,  88, 106};
    const Rgb kBand      { 14,  18,  28};
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
    constexpr int kLogoX    = 2;
    constexpr int kLogoY    = 2;
    constexpr int kSpriteY0 = 35, kSpriteY1 = 51;
    // Text: level with the logo it starts further left than level with the sprite.
    constexpr int kTopX     = 38;                        // rows beside the logo
    constexpr int kTextX    = 48;                        // rows beside the sprite
    constexpr int kRightX   = 125;                       // last usable column
    constexpr int kTopW     = kRightX + 1 - kTopX;       // 88 px
    // Bottom band.
    constexpr int kBandY    = 56;

    constexpr uint32_t kRouteShowMs = 5000;   // route, before switching to the airline name
    constexpr uint32_t kNameShowMs  = 3000;   // airline name
    constexpr uint32_t kSeenShowMs  = 2500;   // registration history, each line
    constexpr uint32_t kFadeMs      = 350;    // cross-fade between the two

    constexpr uint32_t kGlintEveryMs = 7000;   // logo glint cadence
    constexpr uint32_t kGlintMs      = 650;    // glint sweep duration
    constexpr uint32_t kRabbitMs     = 900;    // approach-light run, marker -> threshold
    constexpr float    kMaxAltDeadReckonMs = 60000.0f;

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

    // Airline name short enough for its line: drop generic trailing words
    // ("British Airways Shuttle" -> "British Airways" -> "British"). Returns
    // false if it still doesn't fit, in which case the card keeps the route.
    bool shortAirlineName(const char *name, int maxW, char *out, size_t outLen)
    {
        static const char *const kGeneric[] = {
            "AIRWAYS", "AIRLINES", "AIRLINE", "AIR LINES", "INTERNATIONAL", "CONNECT",
            "SHUTTLE", "DOMESTIC", "EXPRESS", "AVIATION", "GROUP", "REGIONAL", "CARGO",
        };
        char buf[48];
        size_t n = 0;
        for (const char *p = name; *p && n + 1 < sizeof(buf); ++p) buf[n++] = *p;
        buf[n] = 0;
        while (n && buf[n - 1] == ' ') buf[--n] = 0;

        bool trimmed = true;
        while (FrameCanvas::width(Fonts::Label, buf) > maxW && trimmed)
        {
            trimmed = false;
            for (const char *g : kGeneric)
            {
                const size_t gl = strlen(g);
                if (n > gl + 1 && strcasecmp(buf + n - gl, g) == 0 && buf[n - gl - 1] == ' ')
                {
                    n -= gl + 1;
                    buf[n] = 0;
                    trimmed = true;
                    break;
                }
            }
        }
        if (n == 0 || FrameCanvas::width(Fonts::Label, buf) > maxW) return false;
        snprintf(out, outLen, "%s", buf);
        return true;
    }

    // The route line: "LHR -> JFK", or with the city-names setting
    // "Paris -> Rome" ('~' is the Label font's arrow). A city route too long
    // for the line becomes "From New York" / "To New York" when the other end
    // is the home airport (it's on every card, so says nothing), and
    // otherwise scrolls.
    void routeText(const FlightInfo &f, const char *org, const char *dst, char *out, size_t len)
    {
        if (!g_config.route_cities)
        {
            snprintf(out, len, "%s ~ %s", org, dst);
            return;
        }
        const char *oc = Airports::city(f.origin), *dc = Airports::city(f.destination);
        const char *o = oc ? oc : org, *d = dc ? dc : dst;
        snprintf(out, len, "%s ~ %s", o, d);
        if (FrameCanvas::width(Fonts::Label, out) <= kTopW) return;
        if (AirportPack::isHome(f.destination)) snprintf(out, len, "From %s", o);
        else if (AirportPack::isHome(f.origin)) snprintf(out, len, "To %s", d);
    }

    // Scroll for text wider than the line: hold, scroll at 24 px/s, hold.
    constexpr uint32_t kScrollHoldMs = 900;
    uint32_t routeShowMs(int width)
    {
        if (width <= kTopW) return kRouteShowMs;
        return max(kRouteShowMs, 2 * kScrollHoldMs + (uint32_t)(width - kTopW) * 1000 / 24);
    }
    // The route, its arrow in the accent.
    void drawRoute(FrameCanvas &c, int y, const char *s, Rgb colour, Rgb arrow, uint32_t t)
    {
        const int w = FrameCanvas::width(Fonts::Label, s);
        int dx = 0;
        if (w > kTopW)
        {
            const int travel = w - kTopW;
            dx = t < kScrollHoldMs ? 0 : min(travel, (int)((t - kScrollHoldMs) * 24 / 1000));
        }
        c.setClip(kTopX, kRightX);
        const char *a = strchr(s, '~');
        if (a)
        {
            char head[48];
            const size_t n = min((size_t)(a - s), sizeof(head) - 1);
            memcpy(head, s, n);
            head[n] = 0;
            int x = kTopX - dx;
            x += c.print(Fonts::Label, x, y, head, colour);
            x += c.print(Fonts::Label, x, y, "~", arrow);
            c.print(Fonts::Label, x, y, a + 1, colour);
        }
        else
            c.print(Fonts::Label, kTopX - dx, y, s, colour);
        c.resetClip();
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
        char shortCode[5];
        snprintf(shortCode, sizeof(shortCode), "%.*s", f.operator_iata.length() || f.operator_icao.length() ? 4 : 3, code);
        const Fonts::Font &font = FrameCanvas::width(Fonts::Medium, shortCode) <= n - 4 ? Fonts::Medium : Fonts::Label;
        c.print(font, x0 + (n - FrameCanvas::width(font, shortCode)) / 2, y0 + (n - font.cap) / 2, shortCode, kWhite);
    }

    void drawSprite(FrameCanvas &c, const FlightInfo &f, Rgb accent, bool faceRight, uint32_t animMs)
    {
        bool known = false;
        const AircraftSprites::Kind kind = AircraftSprites::classify(f.aircraft_code, known);
        const AircraftSprites::Sprite &sp = AircraftSprites::get(kind);
        const int x = kLeftX + (kLeftW - sp.w) / 2;
        const int y = kSpriteY0 + (kSpriteY1 - kSpriteY0 + 1 - sp.h) / 2;
        AircraftSprites::draw(c, sp, x, y, AircraftSprites::liveryFor(f, accent), faceRight, !known, false, 0,
                              AircraftSprites::Lights, animMs);
    }

    // The approach along the bottom: a 3-degree glide path seen from the side,
    // falling from the right to the runway on the left, with a small
    // aircraft on it (white, its fin in the airline's colour) and the
    // approach lights running ahead of it to the threshold.
    //   ',' fin   '#' fuselage   '+' wing and engine   (nose on the left)
    const char *const kMiniPlane[4] = { ".........,", "........,,", "##########", "...+++...." };
    constexpr int kMiniW = 10, kMiniH = 4;

    void drawStrip(FrameCanvas &c, const ApproachStatus &s, Rgb accent, uint32_t animMs, bool landed)
    {
        // Runway: piano keys at the threshold, the surface after them.
        const int rwyX0 = 52, rwyX1 = 70, groundY = kBandY + 6;
        for (int x = rwyX0; x <= rwyX1; ++x) c.set(x, groundY, Rgb{70, 74, 86});
        for (int x = rwyX0; x <= rwyX0 + 6; x += 2)
            for (int yy = kBandY + 3; yy <= kBandY + 5; ++yy) c.set(x, yy, kBarGrey);

        // The glide path: from the right edge, high, down to the threshold.
        const int pathX0 = 60, pathX1 = 124, pathTop = kBandY;
        auto pathY = [&](float x) { return pathTop + (groundY - 1 - pathTop) * (pathX1 - x) / (pathX1 - pathX0); };

        if (isnan(s.progress) && !landed)
        {
            for (int x = pathX0; x <= pathX1; x += 3) c.set(x, (int)lroundf(pathY(x)), kDimGrey);
            return;
        }
        const float progress = landed ? 1.0f : s.progress;
        const float px = pathX1 - progress * (pathX1 - pathX0);   // the aircraft's nose
        // Flown (behind it, to the right): solid in the accent. Ahead: dotted.
        for (int x = pathX0; x <= pathX1; ++x)
        {
            const int y = (int)lroundf(pathY(x));
            if (x > px + kMiniW)  c.set(x, y, FrameCanvas::scale(accent, 0.3f));
            else if (x < px && (x % 3 == 0)) c.set(x, y, kDimGrey);
        }

        // Approach lights: a "rabbit" running from the aircraft to the threshold.
        if (!landed && s.phase == ApproachStatus::Approach && px - 3 > pathX0)
        {
            const float q = (float)(animMs % kRabbitMs) / kRabbitMs;
            const float x = (px - 3) - q * ((px - 3) - pathX0);
            const int ix = (int)lroundf(x), iy = (int)lroundf(pathY(x));
            c.set(ix, iy, Rgb{255, 255, 255});
            c.set(ix + 1, iy, Rgb{255, 200, 110});
            c.set(ix + 2, iy, Rgb{140, 92, 25});
        }

        // The aircraft: on the path, or on the runway once down.
        const int ox = landed ? rwyX0 + 8 : (int)lroundf(px);   // landed: on the runway, past the keys
        const int oy = landed ? groundY - kMiniH : (int)lroundf(pathY(px + 4)) - 2;
        for (int r = 0; r < kMiniH; ++r)
            for (int col = 0; col < kMiniW; ++col)
            {
                const char ch = kMiniPlane[r][col];
                if (ch == '.') continue;
                const Rgb colr = ch == ',' ? accent : ch == '+' ? Rgb{120, 128, 145} : Rgb{235, 240, 245};
                c.set(ox + col, oy + r, colr);
            }
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
    drawSprite(c, f, accent, opt.spriteRight, opt.animMs);

    // ── Right column ─────────────────────────────────────────────────────────
    // All text goes through fixed buffers: drawing a frame allocates nothing.

    // Flight number: large, or medium when it is too long for the line.
    const char *ident = firstNonEmpty(f.ident_iata, f.ident);
    if (FrameCanvas::width(Fonts::Large, ident) <= kTopW) c.print(Fonts::Large, kTopX, 1, ident, kInk);
    else                                                  c.print(Fonts::Medium, kTopX, 3, ident, kInk);

    // Route line, cross-fading with the shortened airline name (in the
    // accent) and the registration's history. The type row below stays.
    const char *org = firstNonEmpty(f.origin.code_iata, f.origin.code_icao);
    const char *dst = firstNonEmpty(f.destination.code_iata, f.destination.code_icao);
    const bool hasRoute = org[0] && dst[0];
    char route[48] = "", airline[40] = "";
    if (hasRoute) routeText(f, org, dst, route, sizeof(route));
    const bool hasAirline = shortAirlineName(f.airline_display_name_full.c_str(), kTopW, airline, sizeof(airline));
    const uint32_t routeMs = routeShowMs(FrameCanvas::width(Fonts::Label, route));

    // Registration history after the airline: "Seen 14 times", "Last here
    // 3 Oct" or "First visit".
    char seen[24] = "", last[24] = "";
    if (f.seen_days == 1) snprintf(seen, sizeof(seen), "First visit");
    else if (f.seen_days > 1) snprintf(seen, sizeof(seen), "Seen %u times", (unsigned)f.seen_days);
    if (f.seen_days > 1 && f.seen_prev_day)
    {
        // How long ago, in words (8.1): "Last seen yesterday", "4 days ago",
        // "3 weeks ago"; the date once it is more than two months back.
        static const char *const kMon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        const time_t t = (time_t)(18262L + f.seen_prev_day) * 86400L + 43200;   // midday that day
        struct tm lt;
        gmtime_r(&t, &lt);
        long ago = -1;
        const time_t now = time(nullptr);
        if (now > 1600000000)
        {
            struct tm nt;
            localtime_r(&now, &nt);
            // Today's local date as days since 1 Jan 2020, like seen_prev_day.
            const int y = nt.tm_year + 1900;
            const long today = (long)(y - 2020) * 365 + (y - 2017) / 4 - (y - 2001) / 100 + (y - 2001) / 400 + nt.tm_yday;
            ago = today - f.seen_prev_day;
        }
        if (ago == 1)                 snprintf(last, sizeof(last), "Last seen yesterday");
        else if (ago >= 2 && ago < 14) snprintf(last, sizeof(last), "Last seen %ld days ago", ago);
        else if (ago >= 14 && ago < 60) snprintf(last, sizeof(last), "Last seen %ld weeks ago", ago / 7);
        else                          snprintf(last, sizeof(last), "Last here %d %s", lt.tm_mday, kMon[lt.tm_mon]);
    }

    constexpr int kRouteY = 16, kTypeY = 25;
    if (hasRoute && hasAirline)
    {
        const uint32_t seenMs = seen[0] ? kSeenShowMs : 0, lastMs = last[0] ? kSeenShowMs : 0;
        const uint32_t t = opt.animMs % (routeMs + kNameShowMs + seenMs + lastMs);
        const Rgb kSeen{150, 200, 235};
        if (t < routeMs)
        {
            const float k = phaseFade(t, routeMs, kFadeMs);
            drawRoute(c, kRouteY, route, FrameCanvas::scale(kInk, k), FrameCanvas::scale(accent, k), t);
        }
        else if (t < routeMs + kNameShowMs)
            c.print(Fonts::Label, kTopX, kRouteY, airline, FrameCanvas::scale(accent, phaseFade(t - routeMs, kNameShowMs, kFadeMs)));
        else if (t < routeMs + kNameShowMs + seenMs)
            c.print(Fonts::Label, kTopX, kRouteY, seen, FrameCanvas::scale(kSeen, phaseFade(t - routeMs - kNameShowMs, seenMs, kFadeMs)));
        else
            c.print(Fonts::Label, kTopX, kRouteY, last, FrameCanvas::scale(kSeen, phaseFade(t - routeMs - kNameShowMs - seenMs, lastMs, kFadeMs)));
    }
    else if (hasRoute)
        drawRoute(c, kRouteY, route, kInk, accent, opt.animMs % (routeMs + kScrollHoldMs));
    else if (hasAirline)
        c.print(Fonts::Label, kTopX, kRouteY, airline, accent);

    c.setClip(kTopX, kRightX);
    c.print(Fonts::Label, kTopX, kTypeY, firstNonEmpty(f.aircraft_display_name_short, f.aircraft_code), kTypeBlue);
    c.resetClip();

    // Altitude + speed as number and unit (or, once landed, the touchdown
    // time), with a climb/descent arrow when there is room. The altitude
    // comes in already smoothed (opt.altFt); without it, dead-reckon from the
    // vertical rate.
    constexpr int kReadY = 35;
    if (opt.landed)
    {
        int x = kTextX;
        x += c.print(Fonts::Label, x, kReadY + 2, "Landed", kAmber) + 3;
        if (opt.landedAt)
        {
            struct tm lt;
            localtime_r(&opt.landedAt, &lt);
            char hm[8];
            snprintf(hm, sizeof(hm), "%02d:%02d", lt.tm_hour, lt.tm_min);
            c.print(Fonts::Medium, x, kReadY, hm, kInk);
        }
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
        char alt[16] = "", spd[12] = "";
        if (!isnan(a))
        {
            if (a < 0) a = 0;
            withThousands((int)lround(a / 5.0) * 5, alt, sizeof(alt));   // 5 ft steps: reads as a smooth count
        }
        if (!isnan(f.velocity)) snprintf(spd, sizeof(spd), "%d", (int)lround(f.velocity));
        const int spdW = spd[0] ? FrameCanvas::width(Fonts::Medium, spd) + 1 + FrameCanvas::width(Fonts::Label, "kt") : 0;
        const bool arrow = alt[0] && !isnan(f.vertical_rate) && fabs(f.vertical_rate) >= 200;
        int x = kTextX;
        if (alt[0])
        {
            x += c.print(Fonts::Medium, x, kReadY, alt, kInk) + 1;
            x += c.print(Fonts::Label, x, kReadY + 2, "ft", kFaint);
        }
        if (arrow && x + 1 + 5 + 4 + spdW <= kRightX + 1)
        {
            drawBits(c, f.vertical_rate < 0 ? kArrowDown : kArrowUp, 3, x + 1, kReadY + 3, kSoft);
            x += 1 + 5;
        }
        if (spd[0] && x + (alt[0] ? 4 : 0) + spdW <= kRightX + 1)
        {
            x += alt[0] ? 4 : 0;
            x += c.print(Fonts::Medium, x, kReadY, spd, kInk) + 1;
            c.print(Fonts::Label, x, kReadY + 2, "kt", kFaint);
        }
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
            if (mins <= -1)     { snprintf(label, sizeof(label), "%ld min early", -mins); statusCol = kAltGreen; }
            else if (mins >= 1) { snprintf(label, sizeof(label), "%ld min late", mins);   statusCol = kLate; }
            else                { snprintf(label, sizeof(label), "On time");               statusCol = kAltGreen; }
        }
        else
        {
            if (s.runway[0]) snprintf(label, sizeof(label), "Runway %s", s.runway);
            else           { snprintf(label, sizeof(label), "%s", g_airport.name); Fonts::titleCase(label); }
            statusCol = kCyan;
        }
    }
    if (!opt.goAround && !opt.landed) Fonts::sentenceCase(label);   // a go-around stays shouted
    c.setClip(kTextX, kRightX);
    c.print(Fonts::Label, kTextX, 46, label, statusCol);
    c.resetClip();

    // ── Bottom band: ETA / distance, then the approach strip ─────────────────
    {
        const int x0 = opt.border ? 1 : 0, x1 = FrameCanvas::W - 1 - x0, y1 = FrameCanvas::H - 1 - x0;
        for (int y = kBandY - 1; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) c.set(x, y, kBand);
    }
    char eta[16] = "";
    bool isEta = false;
    if (opt.landed)
        ;
    else if (!isnan(s.etaSec))
    {
        const int secs = (int)lroundf(s.etaSec);
        snprintf(eta, sizeof(eta), "%d:%02d", secs / 60, secs % 60);
        isEta = true;
    }
    else if (!isnan(s.distKm))
    {
        if (s.distKm < 10) snprintf(eta, sizeof(eta), "%.1fkm", s.distKm);
        else               snprintf(eta, sizeof(eta), "%dkm", (int)lroundf(s.distKm));
    }
    {
        int x = kLeftX + 1;
        if (isEta) x += c.print(Fonts::Label, x, kBandY, "ETA", kFaint) + 2;
        c.print(Fonts::Label, x, kBandY, eta, kEtaWhite);
    }

    drawStrip(c, s, accent, opt.animMs, opt.landed);
}
