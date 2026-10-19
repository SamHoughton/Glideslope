#include "display/InfoScreens.h"
#include <math.h>
#include <time.h>
#include "config/Airport.h"
#include "config/Airports.h"
#include "config/RuntimeConfig.h"
#include "display/AircraftSprites.h"

namespace
{
    const Rgb kTitle {235, 240, 245};
    const Rgb kLabel { 90, 150, 170};
    const Rgb kValue {255, 185,  60};
    const Rgb kDim   { 60,  66,  80};
    const Rgb kGold  {255, 200,  60};

    void centred(FrameCanvas &c, int y, const char *s, Rgb col, int scale = 1, int pitch = 0)
    {
        c.text((FrameCanvas::W - FrameCanvas::textWidth(s, scale, pitch)) / 2, y, s, col, scale, pitch);
    }

    bool localNow(struct tm &lt)
    {
        const time_t now = time(nullptr);
        if (now < 1600000000) return false;
        localtime_r(&now, &lt);
        return true;
    }
}

void InfoScreens::runwaySummary(const char *runway, char *out, size_t len, bool withSwap)
{
    if (!runway || !runway[0]) { if (len) out[0] = '\0'; return; }
    const bool westerly = runway[0] == '2';
    struct tm lt;
    // Heathrow's westerly ops alternate the landing runway at 15:00 (27L <-> 27R).
    if (withSwap && westerly && g_airport.lhrAlternation && localNow(lt) && lt.tm_hour >= 6 && lt.tm_hour < 15)
        snprintf(out, len, "%s UNTIL 15:00", runway);
    else if (g_airport.lhrAlternation)
        snprintf(out, len, "%s %s ARR", westerly ? "WEST" : "EAST", runway);
    else
        snprintf(out, len, "%s ARRIVALS", runway);
}

namespace
{
    const Rgb kArr {255, 176,  50};   // amber: arrivals
    const Rgb kDep { 60, 200, 225};   // cyan: departures
    const Rgb kSub {120, 170, 210};   // title-bar extras

    float easeOut(float u) { u = u < 0 ? 0 : (u > 1 ? 1 : u); return 1 - (1 - u) * (1 - u) * (1 - u); }
    Rgb mixRgb(Rgb a, Rgb b, float k)
    {
        k = k < 0 ? 0 : (k > 1 ? 1 : k);
        return { (uint8_t)(a.r + (b.r - a.r) * k), (uint8_t)(a.g + (b.g - a.g) * k), (uint8_t)(a.b + (b.b - a.b) * k) };
    }

    // Title band shared by the stats pages: a dark navy gradient, the title
    // on the left and a dim extra (clock, count) on the right.
    void titleBand(FrameCanvas &c, const char *title, const char *extra)
    {
        for (int y = 0; y < 9; ++y)
        {
            const Rgb col = mixRgb(Rgb{14, 22, 48}, Rgb{4, 6, 14}, y / 9.0f);
            for (int x = 0; x < FrameCanvas::W; ++x) c.set(x, y, col);
        }
        c.text(3, 1, title, kTitle);
        if (extra && extra[0]) c.text(FrameCanvas::W - 3 - FrameCanvas::textWidth(extra), 1, extra, kSub);
    }

    // Filled rectangle with the corners left out; edge (if any) along the top.
    void tile(FrameCanvas &c, int x, int y, int w, int h, Rgb fill, const Rgb *edge = nullptr)
    {
        for (int yy = y; yy < y + h; ++yy)
            for (int xx = x; xx < x + w; ++xx)
                if (!((xx == x || xx == x + w - 1) && (yy == y || yy == y + h - 1))) c.set(xx, yy, fill);
        if (edge)
            for (int xx = x + 1; xx < x + w - 1; ++xx) c.set(xx, y, *edge);
    }

    // A horizontal bar brightening towards its end.
    void hbar(FrameCanvas &c, int x, int y, int w, int h, Rgb col)
    {
        for (int i = 0; i < w; ++i)
        {
            const Rgb k = FrameCanvas::scale(col, 0.35f + 0.65f * (i + 1) / w);
            for (int j = 0; j < h; ++j) c.set(x + i, y + j, k);
        }
    }

    // Aircraft families by colour: Airbus narrowbodies blue, widebodies
    // green, A380 gold, Boeing 737 orange, 747 violet, 777 pink, 787 amber,
    // regional jets and props teal.
    Rgb typeColour(const char *t)
    {
        if (!strncmp(t, "A38", 3)) return {255, 225, 90};
        if (t[0] == 'A' && t[1] == '3' && t[2] >= '3' && t[2] <= '5') return {120, 220, 160};   // A330-A350
        if (t[0] == 'A' && (t[1] == '3' || t[1] == '2' || t[1] == '1')) return {90, 170, 255};  // A318-A321, A20N/A21N
        if (!strncmp(t, "B73", 3) || !strncmp(t, "B3", 2)) return {255, 150, 70};
        if (!strncmp(t, "B74", 3)) return {190, 140, 255};
        if (!strncmp(t, "B77", 3)) return {235, 120, 200};
        if (!strncmp(t, "B78", 3)) return {255, 185, 60};
        if (!strncmp(t, "B76", 3)) return {250, 120, 100};
        if (t[0] == 'E' || t[0] == 'C' || !strncmp(t, "AT", 2) || !strncmp(t, "DH", 2)) return {100, 210, 200};
        return {170, 180, 195};
    }
}

// Today: movements in two tiles, an hourly chart of the day (yesterday as a
// faint line to beat, this hour capped white), and a footer cycling through
// the busiest airline, the rarest type, go-arounds and against yesterday.
void InfoScreens::renderStats(FrameCanvas &c, const DailyStats &stats, uint32_t animMs, uint32_t shownMs)
{
    const float count = easeOut(shownMs / 900.0f);
    c.clear();
    struct tm lt;
    const bool haveTime = localNow(lt);
    char title[20], clock[8] = "";
    static const char *const kDays[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    if (haveTime)
    {
        snprintf(title, sizeof(title), "TODAY %s %d", kDays[lt.tm_wday], lt.tm_mday);
        snprintf(clock, sizeof(clock), "%02d:%02d", lt.tm_hour, lt.tm_min);
    }
    else
        snprintf(title, sizeof(title), "TODAY");
    titleBand(c, title, clock);

    // Tiles: the count, its label, and the last full hour's movements.
    const int nowH = haveTime ? lt.tm_hour : -1;
    for (int side = 0; side < 2; ++side)
    {
        const bool arr = side == 0;
        const int x = arr ? 2 : 65;
        const Rgb col = arr ? kArr : kDep;
        const Rgb edge = FrameCanvas::scale(col, 0.55f);
        tile(c, x, 11, 61, 21, arr ? Rgb{44, 27, 6} : Rgb{6, 32, 40}, &edge);
        char v[12];
        const int total = arr ? stats.arrivals() : stats.departures();
        snprintf(v, sizeof(v), "%d", (int)lroundf(total * count));
        const int pitch = total >= 100 ? 10 : 11;   // three digits a little tighter
        c.text(x + 4, 15, v, col, 2, pitch);
        snprintf(v, sizeof(v), "%d", total);         // its final width, so nothing moves as it counts
        const int numRight = x + 4 + FrameCanvas::textWidth(v, 2, pitch);
        const char *label = arr ? "ARR" : "DEP";
        c.text(x + 59 - FrameCanvas::textWidth(label), 14, label, FrameCanvas::scale(col, 0.75f));
        const int lastH = nowH > 0 ? (arr ? stats.arrivalsInHour(nowH - 1) : stats.departuresInHour(nowH - 1)) : 0;
        snprintf(v, sizeof(v), "%d/H", lastH);
        const int rateX = x + 59 - FrameCanvas::textWidth(v);
        if (lastH > 0 && rateX >= numRight + 2)   // only where it clears the number
            c.text(rateX, 23, v, FrameCanvas::scale(col, 0.5f));
    }

    // Hourly chart: 24 bars of 4 px, departures stacked on arrivals, shaded
    // brighter towards the top, scaled to the busiest hour (today or yesterday).
    constexpr int kTop = 35, kBase = 53, kX0 = 4;
    int peak = 1;
    for (int h = 0; h < 24; ++h)
        peak = max(peak, max(stats.arrivalsInHour(h) + stats.departuresInHour(h), stats.yesterdayInHour(h)));
    const float pulse = 0.7f + 0.3f * sinf(animMs / 250.0f);
    for (int h = 0; h < 24; ++h)
    {
        const int x = kX0 + h * 5;
        const int a = stats.arrivalsInHour(h), d = stats.departuresInHour(h);
        const float grow = easeOut((shownMs - h * 18.0f) / 450.0f);   // bars rise left to right
        const int ha = (int)(((a * (kBase - kTop) + peak - 1) / peak) * grow);
        const int hd = (int)(((d * (kBase - kTop) + peak - 1) / peak) * grow);
        for (int i = 0; i < ha; ++i)
        {
            const Rgb k = FrameCanvas::scale(kArr, 0.45f + 0.55f * (i + 1) / ha);
            for (int dx = 0; dx < 4; ++dx) c.set(x + dx, kBase - 1 - i, k);
        }
        for (int i = 0; i < hd; ++i)
        {
            const Rgb k = FrameCanvas::scale(kDep, 0.45f + 0.55f * (i + 1) / hd);
            for (int dx = 0; dx < 4; ++dx) c.set(x + dx, kBase - 1 - ha - i, k);
        }
        // Yesterday's total for the hour, where the bar hasn't covered it.
        const int yh = stats.yesterdayInHour(h);
        if (yh)
        {
            const int y = kBase - 1 - (yh * (kBase - kTop) + peak - 1) / peak;
            for (int dx = 0; dx < 4; ++dx)
                if (c.get(x + dx, y) == 0) c.set(x + dx, y, Rgb{55, 60, 85});
        }
        if (h == nowH && ha + hd)
            for (int dx = 0; dx < 4; ++dx) c.set(x + dx, kBase - 1 - ha - hd, FrameCanvas::scale(kTitle, pulse));
        // Baseline ticks, brighter at 00, 06, 12, 18.
        const Rgb tick = (h % 6 == 0) ? kLabel : Rgb{35, 40, 52};
        c.set(x + 1, kBase, tick);
        c.set(x + 2, kBase, tick);
    }

    // Footer: a chip (in the fact's colour) and the fact, one at a time,
    // each for 3 s with a fade between.
    struct Fact { char chip[5]; Rgb col; char text[17]; };
    Fact facts[4];
    int nf = 0, n = 0;
    const String airline = stats.busiestAirline(n);
    if (airline.length())
    {
        Fact &f = facts[nf++];
        strlcpy(f.chip, airline.c_str(), sizeof(f.chip));
        f.col = AircraftSprites::airlineColour(airline.c_str());
        snprintf(f.text, sizeof(f.text), "BUSIEST  %d", n);
    }
    const String rare = stats.rarestType();
    if (rare.length())
    {
        Fact &f = facts[nf++];
        strlcpy(f.chip, "RARE", sizeof(f.chip));
        f.col = kGold;
        snprintf(f.text, sizeof(f.text), "%s", rare.c_str());
    }
    if (stats.goArounds() > 0)
    {
        Fact &f = facts[nf++];
        strlcpy(f.chip, "GA", sizeof(f.chip));
        f.col = Rgb{255, 95, 80};
        snprintf(f.text, sizeof(f.text), "GO-AROUNDS %d", stats.goArounds());
    }
    if (haveTime && stats.hasYesterday())
    {
        const int then = stats.yesterdayUpTo(lt.tm_hour, lt.tm_min);
        const int nowN = stats.arrivals() + stats.departures();
        if (then >= 20)
        {
            const int pct = (int)lroundf((nowN - then) * 100.0f / then);
            Fact &f = facts[nf++];
            snprintf(f.chip, sizeof(f.chip), "%s%d%%", pct >= 0 ? "+" : "", max(-99, min(99, pct)));
            f.col = pct >= 0 ? Rgb{90, 200, 110} : Rgb{235, 100, 90};
            snprintf(f.text, sizeof(f.text), "VS YESTERDAY");
        }
    }
    if (nf)
    {
        const uint32_t per = 3000;
        const int i = (animMs / per) % nf;
        const uint32_t t = animMs % per;
        const float fade = nf > 1 ? (t < 300 ? t / 300.0f : (t > per - 300 ? (per - t) / 300.0f : 1.0f)) : 1.0f;
        const Fact &f = facts[i];
        tile(c, 2, 55, 124, 9, FrameCanvas::scale(Rgb{10, 22, 48}, 1));
        tile(c, 2, 55, 26, 9, FrameCanvas::scale(mixRgb(Rgb{0, 0, 0}, f.col, 0.55f), fade));
        c.text(2 + (26 - FrameCanvas::textWidth(f.chip)) / 2, 56, f.chip, FrameCanvas::scale(kTitle, fade));
        c.text(32, 56, f.text, FrameCanvas::scale(mixRgb(f.col, kTitle, 0.45f), fade));
    }
}

// The day's busiest airlines: code and a bar in the airline's colour, growing in.
void InfoScreens::renderTopAirlines(FrameCanvas &c, const DailyStats &stats, uint32_t shownMs)
{
    c.clear();
    DailyStats::Top top[5];
    const int n = stats.topAirlines(top, 5);
    char extra[8];
    snprintf(extra, sizeof(extra), "%d", stats.airlineCount());
    titleBand(c, "TOP AIRLINES", extra);
    const int most = n ? max(1, top[0].n) : 1;
    for (int i = 0; i < n; ++i)
    {
        const int y = 12 + i * 10;
        const Rgb col = AircraftSprites::airlineColour(top[i].code);
        c.text(3, y, top[i].code, col);
        const float grow = easeOut((shownMs - i * 120.0f) / 600.0f);
        const int w = max(2, (int)lroundf(top[i].n * 82.0f / most * grow));
        hbar(c, 24, y, w, 7, col);
        char v[8];
        snprintf(v, sizeof(v), "%d", (int)lroundf(top[i].n * grow));
        c.text(24 + w + 3, y, v, kTitle);
    }
}

// The day's commonest aircraft types, coloured by family.
void InfoScreens::renderTopTypes(FrameCanvas &c, const DailyStats &stats, uint32_t shownMs)
{
    c.clear();
    DailyStats::Top top[5];
    const int n = stats.topTypes(top, 5);
    char extra[8];
    snprintf(extra, sizeof(extra), "%d", stats.typeCount());
    titleBand(c, "TYPES TODAY", extra);
    const int most = n ? max(1, top[0].n) : 1;
    for (int i = 0; i < n; ++i)
    {
        const int y = 12 + i * 10;
        const Rgb col = typeColour(top[i].code);
        c.text(3, y, top[i].code, col, 1, 0, 4);
        const float grow = easeOut((shownMs - i * 120.0f) / 600.0f);
        const int w = max(2, (int)lroundf(top[i].n * 70.0f / most * grow));
        hbar(c, 30, y + 1, w, 5, col);
        char v[8];
        snprintf(v, sizeof(v), "%d", (int)lroundf(top[i].n * grow));
        c.text(30 + w + 3, y, v, kTitle);
    }
}

void InfoScreens::renderClock(FrameCanvas &c, uint32_t animMs, const char *weather)
{
    c.clear();
    struct tm lt;
    if (!localNow(lt))
    {
        centred(c, 28, "--:--", kDim, 2);
        return;
    }
    // HH:MM at triple size with a narrow colon (the font's colon is a full
    // character wide); the colon pulses gently with the seconds.
    const Rgb digits{120, 130, 150};
    char hh[3], mm[3];
    snprintf(hh, sizeof(hh), "%02d", lt.tm_hour);
    snprintf(mm, sizeof(mm), "%02d", lt.tm_min);
    constexpr int kPitch = 17, kDigitW = 15, kGap = 4, kColonW = 3, kTop = 16;
    const int total = 2 * (kPitch + kDigitW) + 2 * kGap + kColonW;
    const int x0 = (FrameCanvas::W - total) / 2;
    c.text(x0, kTop, hh, digits, 3, kPitch);
    const int cx = x0 + kPitch + kDigitW + kGap;
    const float pulse = 0.35f + 0.65f * (0.5f + 0.5f * cosf((animMs % 2000) * (float)M_PI / 1000.0f));
    c.fillRect(cx, kTop + 5, kColonW, kColonW, FrameCanvas::scale(digits, pulse));
    c.fillRect(cx, kTop + 13, kColonW, kColonW, FrameCanvas::scale(digits, pulse));
    c.text(cx + kColonW + kGap, kTop, mm, digits, 3, kPitch);
    // Tonight's moon, top right, in its real phase.
    Sky::drawMoon(c, FrameCanvas::W - 9, 7, 4.2f, Sky::moonPhase(time(nullptr)), Rgb{150, 150, 130});
    static const char *const kMonths[] = {"JAN","FEB","MAR","APR","MAY","JUN","JUL","AUG","SEP","OCT","NOV","DEC"};
    char date[16];
    snprintf(date, sizeof(date), "%d %s", lt.tm_mday, kMonths[lt.tm_mon]);
    centred(c, 44, date, kDim);
    if (weather && weather[0]) centred(c, 55, weather, Rgb{40, 46, 58});
}

namespace
{
    // Split-flap text, like an old station board: each letter flips through
    // others before landing, left to right, starting at delayMs.
    void flapText(FrameCanvas &c, int x, int y, const char *text, Rgb col, uint32_t shownMs, uint32_t delayMs)
    {
        static const char kFlaps[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        for (int i = 0; text[i]; ++i)
        {
            const int cx = x + i * 6;
            const uint32_t settle = delayMs + 160 + i * 45;
            char ch[2] = {text[i], 0};
            if (shownMs >= settle || text[i] == ' ')
            {
                c.text(cx, y, ch, col);
                continue;
            }
            if (shownMs + 200 < settle - 160) continue;   // not started: blank
            uint32_t h = (shownMs / 55) * 2654435761u + i * 40503u + y * 131u;
            h ^= h >> 13;
            ch[0] = kFlaps[h % (sizeof(kFlaps) - 1)];
            c.text(cx, y, ch, FrameCanvas::scale(col, 0.55f));
            for (int dx = 0; dx < 5; ++dx) c.set(cx + dx, y + 3, Rgb{0, 0, 0});   // the split
        }
    }
}

void InfoScreens::renderArrivals(FrameCanvas &c, const Arrival *rows, int n, const char *runway,
                                 const char *weather, unsigned long nowMs, uint32_t shownMs)
{
    c.clear();
    char title[24];
    if (runway && runway[0]) snprintf(title, sizeof(title), "ARRIVALS %s", runway);
    else                     snprintf(title, sizeof(title), "ARRIVALS");
    centred(c, 2, title, kTitle);
    for (int x = 8; x < FrameCanvas::W - 8; x += 2) c.set(x, 11, kDim);

    if (n == 0) centred(c, 28, "NONE INBOUND", kDim);
    for (int i = 0; i < n && i < kMaxArrivals; ++i)
    {
        const Arrival &a = rows[i];
        const int y = 14 + i * 10;
        c.fillRect(2, y, 2, 7, a.accent);                       // airline colour tab
        flapText(c, 6, y, a.ident, Rgb{235, 240, 245}, shownMs, i * 110);
        flapText(c, 54, y, a.type, kLabel, shownMs, i * 110 + 200);

        // Count down between fetches.
        char eta[12];
        float left = a.etaSec - (nowMs - a.dataMs) / 1000.0f;
        if (isnan(left)) snprintf(eta, sizeof(eta), "-");
        else if (left < 60) snprintf(eta, sizeof(eta), a.estimate ? "~1 MIN" : "<1 MIN");
        else snprintf(eta, sizeof(eta), "%s%d MIN", a.estimate ? "~" : "", (int)lroundf(left / 60.0f));
        flapText(c, FrameCanvas::W - 3 - FrameCanvas::textWidth(eta), y, eta,
                 !a.estimate && left < 120 ? kValue : Rgb{200, 205, 215}, shownMs, i * 110 + 350);
    }

    // Weather along the bottom: what the arrivals are landing into.
    if (weather && weather[0])
    {
        for (int x = 8; x < FrameCanvas::W - 8; x += 2) c.set(x, 54, kDim);
        centred(c, 56, weather, Rgb{120, 160, 190});
    }
}

namespace
{
    // A windsock on a pole, seen side on: limp in calm air, straight out in
    // 15 knots or more, pointing where the wind blows (east to the right).
    void drawWindsock(FrameCanvas &c, int px, int groundY, const Metar &m, unsigned long nowMs, float light)
    {
        const int top = groundY - 18;
        const Rgb pole = FrameCanvas::scale(Rgb{150, 155, 165}, light);
        for (int y = top; y < groundY; ++y) c.set(px, y, pole);
        float lift = m.windKt <= 0 ? 0.0f : min(1.0f, m.windKt / 15.0f);
        // Gusts make it lift and fall.
        if (m.gustKt) lift = min(1.0f, lift + 0.15f * (0.5f + 0.5f * sinf(nowMs / 230.0f)));
        float side = 1;
        if (m.windDir >= 0)
        {
            const float to = (m.windDir + 180) * (float)M_PI / 180.0f;
            side = sinf(to) >= 0 ? 1.0f : -1.0f;
            lift *= max(0.45f, fabsf(sinf(to)));   // blowing towards or away from us: foreshortened
        }
        const float ang = (1 - lift) * 1.35f + sinf(nowMs / 170.0f) * 0.05f * lift;   // radians below horizontal
        const float ux = cosf(ang) * side, uy = sinf(ang);
        const Rgb orange = FrameCanvas::scale(Rgb{255, 110, 30}, light), white = FrameCanvas::scale(Rgb{235, 235, 235}, light);
        for (int i = 0; i <= 13; ++i)
        {
            const float x = px + ux * i, y = top + 1 + uy * i;
            const Rgb col = (i / 3) % 2 ? white : orange;
            const int half = i < 5 ? 1 : 0;   // wide at the mouth, narrow at the tail
            for (int w = -half; w <= half; ++w)
                c.set((int)lroundf(x - uy * w * side), (int)lroundf(y + ux * w * side), col);
        }
    }

    // Top-down: the runway in use and the wind blowing across it.
    void drawRunwayWind(FrameCanvas &c, int cx, int cy, int r, const Metar &m, const char *runway, unsigned long nowMs)
    {
        for (int a = 0; a < 360; a += 30)
        {
            const float rad = a * (float)M_PI / 180.0f;
            c.set(cx + (int)lroundf(sinf(rad) * r), cy - (int)lroundf(cosf(rad) * r), a == 0 ? kTitle : kDim);
        }
        if (runway && runway[0])
        {
            const float rh = atoi(runway) * 10 * (float)M_PI / 180.0f;
            const float dx = sinf(rh), dy = -cosf(rh);
            for (int i = -(r - 2); i <= r - 2; ++i)
                for (int w = -1; w <= 1; ++w)
                    c.set(cx + (int)lroundf(dx * i - dy * w * 0.8f), cy + (int)lroundf(dy * i + dx * w * 0.8f),
                          w == 0 && (i & 1) ? Rgb{200, 200, 205} : Rgb{70, 74, 84});
        }
        if (m.windDir >= 0 && m.windKt > 0)
        {
            // From the side the wind comes from towards the middle, moving.
            const float from = m.windDir * (float)M_PI / 180.0f;
            const float dx = sinf(from), dy = -cosf(from);
            const float shift = fmodf(nowMs / 90.0f, 4.0f);
            for (float i = r + 3 - shift; i > 1; i -= 4)
                for (float j = 0; j < 2; j += 1)
                    c.set(cx + (int)lroundf(dx * (i - j)), cy + (int)lroundf(dy * (i - j)), kValue);
            for (int k = 1; k <= 2; ++k)
                for (int s = -1; s <= 1; s += 2)
                    c.set(cx + (int)lroundf(dx * (1 + k) - dy * k * s), cy + (int)lroundf(dy * (1 + k) + dx * k * s), kValue);
        }
    }
}

void InfoScreens::renderDepartures(FrameCanvas &c, const Departure *rows, int n, const char *runways, uint32_t shownMs)
{
    c.clear();
    char title[24];
    if (runways && runways[0]) snprintf(title, sizeof(title), "DEPARTED %s", runways);
    else                       snprintf(title, sizeof(title), "DEPARTED");
    centred(c, 2, title, kTitle);
    for (int x = 8; x < FrameCanvas::W - 8; x += 2) c.set(x, 11, kDim);
    if (n == 0) { centred(c, 28, "NONE RECENTLY", kDim); return; }
    for (int i = 0; i < n && i < kMaxDepartures; ++i)
    {
        const Departure &d = rows[i];
        const int y = 14 + i * 10;
        c.fillRect(2, y, 2, 7, d.accent);
        flapText(c, 6, y, d.ident, Rgb{235, 240, 245}, shownMs, i * 110);
        char city[8];
        const char *dest = g_config.route_cities && Airports::shortCity(d.dest, 7, city, sizeof(city)) ? city : d.dest;
        if (d.dest[0]) flapText(c, 54, y, dest, kValue, shownMs, i * 110 + 200);
        else           flapText(c, 54, y, d.type, kLabel, shownMs, i * 110 + 200);
        char when[8] = "";
        if (d.at)
        {
            struct tm lt;
            localtime_r(&d.at, &lt);
            snprintf(when, sizeof(when), "%02d:%02d", lt.tm_hour, lt.tm_min);
        }
        flapText(c, FrameCanvas::W - 3 - FrameCanvas::textWidth(when), y, when, Rgb{200, 205, 215}, shownMs, i * 110 + 350);
    }
}

void InfoScreens::renderWeather(FrameCanvas &c, const Metar &m, const char *runway, unsigned long nowMs,
                                const Sky::Look &skyNow)
{
    c.clear();
    if (!m.valid)
    {
        centred(c, 2, "WEATHER", kTitle);
        centred(c, 30, "NO REPORT YET", kDim);
        return;
    }

    // Left: the scene. Sky (clipped to the left half), ground, windsock,
    // rain or snow, and the temperature on the grass.
    constexpr int kSceneW = 62, kGroundY = 50;
    const Sky::Look sky = skyNow.known ? skyNow : Sky::preview("day");
    c.setClip(0, kSceneW - 1);
    Sky::drawSky(c, sky, nowMs, kGroundY);
    const float light = 1.0f - 0.55f * sky.night;
    const Rgb grass = FrameCanvas::scale(sky.precip == Sky::Snow ? Rgb{170, 175, 185} : Rgb{34, 62, 36}, light);
    c.fillRect(0, kGroundY, kSceneW, FrameCanvas::H - kGroundY, grass);
    drawWindsock(c, 47, kGroundY, m, nowMs, max(light, 0.6f));
    Sky::drawWeather(c, sky, nowMs, 0, kSceneW - 1);
    c.resetClip();
    if (m.tempC != -99)
    {
        char t[8];
        snprintf(t, sizeof(t), "%dC", m.tempC);
        c.text(3, kGroundY + 1, t, Rgb{0, 0, 0}, 2, 11);
        c.text(2, kGroundY, t, m.tempC <= 0 ? Rgb{150, 200, 255} : m.tempC >= 25 ? Rgb{255, 150, 80} : kTitle, 2, 11);
    }
    for (int y = 0; y < FrameCanvas::H; ++y) c.set(kSceneW, y, Rgb{0, 0, 0});

    // Right: the numbers.
    constexpr int kX = 65, kRW = FrameCanvas::W - kX;
    auto rightCentred = [&](int y, const char *s, Rgb col) {
        c.text(kX + (kRW - FrameCanvas::textWidth(s)) / 2, y, s, col);
    };
    rightCentred(1, FrameCanvas::textWidth(g_airport.name) <= kRW ? g_airport.name : g_airport.icao, kTitle);
    for (int x = kX + 2; x < FrameCanvas::W - 2; x += 2) c.set(x, 10, kDim);

    char wind[16];
    if (m.windKt == 0)      snprintf(wind, sizeof(wind), "CALM");
    else if (m.windDir < 0) snprintf(wind, sizeof(wind), "VRB %dKT", m.windKt);
    else                    snprintf(wind, sizeof(wind), "%03d/%dKT", m.windDir, m.windKt);
    c.text(kX + 1, 13, wind, kValue);
    if (m.gustKt)
    {
        char g[8];
        snprintf(g, sizeof(g), "G%d", m.gustKt);
        c.text(FrameCanvas::W - 2 - FrameCanvas::textWidth(g), 13, g, Rgb{255, 95, 80});
    }

    drawRunwayWind(c, kX + 11, 32, 9, m, runway, nowMs);
    if (runway && runway[0])
    {
        c.text(kX + 26, 21, runway, kLabel);
        if (m.windDir >= 0 && m.windKt > 0)
        {
            const int rwyDeg = atoi(runway) * 10;   // "27L" -> 270
            int cross = 0, tail = 0;
            Weather::components(m, rwyDeg, cross, tail);
            char xw[10], hw[10];
            snprintf(xw, sizeof(xw), "XW %d", cross);
            snprintf(hw, sizeof(hw), "%s %d", tail > 0 ? "TW" : "HW", tail > 0 ? tail : -tail);
            c.text(kX + 26, 29, xw, cross >= 20 ? Rgb{255, 95, 80} : Rgb{200, 205, 215});
            c.text(kX + 26, 37, hw, tail >= 5 ? Rgb{255, 95, 80} : Rgb{200, 205, 215});
        }
    }

    char row[24], vis[8] = "-";
    if (m.visText[0]) strlcpy(vis, m.visText, sizeof(vis));
    snprintf(row, sizeof(row), "VIS %s", vis);
    c.text(kX + 1, 47, row, Rgb{200, 205, 215});
    char pr[10];
    Weather::pressure(m, pr, sizeof(pr));
    snprintf(row, sizeof(row), "%s%s%s", pr, m.wx[0] ? " " : "", m.wx);
    c.text(kX + 1, 56, row, kLabel);
}

void InfoScreens::renderAlert(FrameCanvas &c, const char *code, const char *meaning, const char *ident,
                              const char *detail, uint32_t tMs)
{
    c.clear();
    const bool on = (tMs / 300) % 2 == 0;
    const Rgb red{255, 40, 30};
    // Flashing red frame, two pixels thick.
    const Rgb frame = on ? red : Rgb{90, 10, 8};
    for (int i = 0; i < 2; ++i)
    {
        for (int x = 0; x < FrameCanvas::W; ++x) { c.set(x, i, frame); c.set(x, FrameCanvas::H - 1 - i, frame); }
        for (int y = 0; y < FrameCanvas::H; ++y) { c.set(i, y, frame); c.set(FrameCanvas::W - 1 - i, y, frame); }
    }
    char sq[16];
    snprintf(sq, sizeof(sq), "SQUAWK %s", code);
    centred(c, 5, sq, on ? Rgb{255, 255, 255} : red);
    const int w = FrameCanvas::textWidth(meaning, 2, 11);
    if (w <= FrameCanvas::W - 6) c.text((FrameCanvas::W - w) / 2, 16, meaning, red, 2, 11);
    else                         centred(c, 20, meaning, red);
    centred(c, 36, ident, kTitle);
    centred(c, 48, detail, Rgb{200, 205, 215});
}

void InfoScreens::renderRareBanner(FrameCanvas &c, const char *line1, const char *line2, uint32_t tMs)
{
    c.clear();
    // Twinkling gold stars scattered around the text.
    static const int8_t kStars[][2] = {
        {6, 6}, {20, 54}, {34, 10}, {58, 57}, {76, 5}, {94, 52}, {110, 9}, {121, 40}, {8, 34}, {116, 24},
    };
    for (int i = 0; i < 10; ++i)
    {
        const float k = 0.5f + 0.5f * sinf(tMs / 90.0f + i * 1.3f);
        const Rgb col = FrameCanvas::scale(kGold, k);
        const int x = kStars[i][0], y = kStars[i][1];
        c.set(x, y, col);
        if (k > 0.7f) { c.set(x - 1, y, FrameCanvas::scale(col, 0.4f)); c.set(x + 1, y, FrameCanvas::scale(col, 0.4f));
                        c.set(x, y - 1, FrameCanvas::scale(col, 0.4f)); c.set(x, y + 1, FrameCanvas::scale(col, 0.4f)); }
    }
    // Title flashes white/gold, the subject stays steady.
    const Rgb titleCol = (tMs / 200) % 2 ? kGold : Rgb{255, 255, 255};
    centred(c, 14, line1, titleCol);
    const int w2 = FrameCanvas::textWidth(line2, 2, 11);
    if (w2 <= FrameCanvas::W - 4) c.text((FrameCanvas::W - w2) / 2, 28, line2, kGold, 2, 11);
    else                         centred(c, 32, line2, kGold);
}

void InfoScreens::renderRunwayChange(FrameCanvas &c, const char *from, const char *to, uint32_t tMs)
{
    c.clear();
    // An amber frame that pulses, the new runway large in the middle.
    const float k = 0.55f + 0.45f * sinf(tMs / 160.0f);
    const Rgb frame = FrameCanvas::scale(kValue, k);
    for (int x = 0; x < FrameCanvas::W; ++x) { c.set(x, 0, frame); c.set(x, FrameCanvas::H - 1, frame); }
    for (int y = 0; y < FrameCanvas::H; ++y) { c.set(0, y, frame); c.set(FrameCanvas::W - 1, y, frame); }
    centred(c, 5, "RUNWAY CHANGE", (tMs / 400) % 2 ? kValue : kTitle);
    centred(c, 16, "NOW LANDING", kLabel);
    const int w = FrameCanvas::textWidth(to, 2, 11);
    c.text((FrameCanvas::W - w) / 2, 27, to, kValue, 2, 11);
    if (from && from[0])
    {
        char was[12];
        snprintf(was, sizeof(was), "WAS %s", from);
        centred(c, 52, was, Rgb{140, 146, 160});
    }
}

void InfoScreens::renderHolding(FrameCanvas &c, const HoldTracker::Row *rows, int n, uint32_t animMs)
{
    c.clear();
    int total = 0;
    for (int i = 0; i < n; ++i) total += rows[i].count;
    char title[20];
    snprintf(title, sizeof(title), total == 1 ? "1 HOLDING" : "%d HOLDING", total);
    centred(c, 1, title, kTitle);
    for (int x = 4; x < FrameCanvas::W - 4; ++x) c.set(x, 10, kDim);

    const int pitch = n > 4 ? 10 : 12;
    const int y0 = n > 4 ? 13 : 14;
    for (int i = 0; i < n && i < 5; ++i)
    {
        const HoldTracker::Row &r = rows[i];
        const int y = y0 + i * pitch;
        const bool any = r.count > 0;
        c.text(2, y, r.name, any ? kLabel : kDim);
        char num[6];
        snprintf(num, sizeof(num), "%u", (unsigned)r.count);
        c.text(42 - FrameCanvas::textWidth(num), y, num, any ? kValue : kDim);

        // The racetrack: two straights and two half circles, aircraft
        // spaced evenly around it, all going round once every 8 s.
        const float cx = 72, cy = y + 3, L = 26, rad = 3;
        const float P = 2 * L + 2 * (float)M_PI * rad;
        auto at = [&](float p, int &px, int &py) {
            float s = fmodf(p, 1.0f) * P;
            float x, yy;
            if (s < L)                      { x = cx - L / 2 + s; yy = cy - rad; }
            else if (s < L + M_PI * rad)    { const float t = -M_PI / 2 + (s - L) / rad; x = cx + L / 2 + rad * cosf(t); yy = cy + rad * sinf(t); }
            else if (s < 2 * L + M_PI * rad){ x = cx + L / 2 - (s - L - M_PI * rad); yy = cy + rad; }
            else                            { const float t = M_PI / 2 + (s - 2 * L - M_PI * rad) / rad; x = cx - L / 2 + rad * cosf(t); yy = cy + rad * sinf(t); }
            px = (int)lroundf(x); py = (int)lroundf(yy);
        };
        for (int k = 0; k < 64; ++k)
        {
            int px, py;
            at(k / 64.0f, px, py);
            c.set(px, py, any ? Rgb{34, 40, 54} : Rgb{20, 23, 30});
        }
        const int shown = min((int)r.count, 8);
        for (int k = 0; k < shown; ++k)
        {
            int px, py;
            const float p = animMs / 8000.0f + (float)k / shown;
            for (int t = 3; t >= 1; --t)   // a short fading trail behind each aircraft
            {
                at(p + 1.0f - t * 0.012f, px, py);
                c.set(px, py, FrameCanvas::scale(kValue, 0.5f - t * 0.12f));
            }
            at(p, px, py);
            c.set(px, py, Rgb{255, 235, 190});
        }

        if (any)
        {
            char mins[8];
            snprintf(mins, sizeof(mins), r.longestMin ? "%uM" : "<1M", (unsigned)r.longestMin);
            c.text(FrameCanvas::W - 2 - FrameCanvas::textWidth(mins), y, mins, kTitle);
        }
    }
}

void InfoScreens::renderCaption(FrameCanvas &c, const char *text)
{
    c.clear();
    centred(c, 28, text, kTitle);
}
