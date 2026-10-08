#include "display/InfoScreens.h"
#include <math.h>
#include <time.h>
#include "config/Airport.h"

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
    const Rgb kArr {255, 185,  60};   // amber: arrivals
    const Rgb kDep { 70, 200, 220};   // cyan: departures

    // 7x7 arrows beside the big counters.
    void arrow(FrameCanvas &c, int x, int y, bool down, Rgb col)
    {
        // Three rows of shaft, then the head narrowing to the tip.
        for (int r = 0; r < 7; ++r)
        {
            const int row = down ? r : 6 - r;
            const int w = r < 3 ? 0 : 6 - r;
            for (int dx = -w; dx <= w; ++dx)
            {
                c.set(x + 3 + dx, y + row, col);
                if (r < 3) c.set(x + 4, y + row, col);   // a two-pixel shaft
            }
        }
    }
}

// Today: big arrival and departure counters, an hourly bar chart of the
// day's movements, and a footer cycling through the busiest airline, the
// rarest type and go-arounds.
void InfoScreens::renderStats(FrameCanvas &c, const DailyStats &stats, uint32_t animMs)
{
    c.clear();
    struct tm lt;
    const bool haveTime = localNow(lt);
    char title[20];
    static const char *const kDays[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    if (haveTime) snprintf(title, sizeof(title), "TODAY %s %d", kDays[lt.tm_wday], lt.tm_mday);
    else          snprintf(title, sizeof(title), "TODAY");
    centred(c, 1, title, kTitle);

    // Counters: arrow + number (double size), label underneath.
    char v[12];
    snprintf(v, sizeof(v), "%d", stats.arrivals());
    arrow(c, 2, 15, true, kArr);
    c.text(12, 11, v, kArr, 2, 11);
    c.text(2, 28, "ARRIVALS", FrameCanvas::scale(kArr, 0.55f));
    snprintf(v, sizeof(v), "%d", stats.departures());
    arrow(c, 66, 15, false, kDep);
    c.text(76, 11, v, kDep, 2, 11);
    c.text(66, 28, "DEPARTURES", FrameCanvas::scale(kDep, 0.55f));

    // Hourly chart: 24 bars of 4 px (plus a 1 px gap), departures stacked on
    // arrivals, scaled to the busiest hour. The current hour pulses.
    constexpr int kTop = 37, kBase = 51, kX0 = 4;
    int peak = 1;
    for (int h = 0; h < 24; ++h) peak = max(peak, stats.arrivalsInHour(h) + stats.departuresInHour(h));
    const int nowH = haveTime ? lt.tm_hour : -1;
    const float pulse = 0.65f + 0.35f * sinf(animMs / 250.0f);
    for (int h = 0; h < 24; ++h)
    {
        const int x = kX0 + h * 5;
        const int a = stats.arrivalsInHour(h), d = stats.departuresInHour(h);
        const int ha = (a * (kBase - kTop) + peak - 1) / peak;
        const int hd = (d * (kBase - kTop) + peak - 1) / peak;
        const float k = h == nowH ? pulse : (h > nowH && nowH >= 0 ? 0.0f : 0.8f);
        for (int y = 0; y < ha; ++y)
            for (int dx = 0; dx < 4; ++dx) c.set(x + dx, kBase - 1 - y, FrameCanvas::scale(kArr, k));
        for (int y = 0; y < hd; ++y)
            for (int dx = 0; dx < 4; ++dx) c.set(x + dx, kBase - 1 - ha - y, FrameCanvas::scale(kDep, k));
        // Baseline dot for every hour; brighter ticks at 06, 12, 18.
        c.set(x + 1, kBase, (h % 6 == 0) ? kLabel : kDim);
        c.set(x + 2, kBase, (h % 6 == 0) ? kLabel : kDim);
    }

    // Footer: one fact at a time, each for 3 s, fading between.
    char facts[3][24];
    int nf = 0, n = 0;
    const String airline = stats.busiestAirline(n);
    if (airline.length()) snprintf(facts[nf++], sizeof(facts[0]), "BUSIEST %s %d", airline.c_str(), n);
    const String rare = stats.rarestType();
    if (rare.length()) snprintf(facts[nf++], sizeof(facts[0]), "RAREST %s", rare.c_str());
    if (stats.goArounds() > 0) snprintf(facts[nf++], sizeof(facts[0]), "GO-AROUNDS %d", stats.goArounds());
    if (nf)
    {
        const uint32_t per = 3000;
        const int i = (animMs / per) % nf;
        const uint32_t t = animMs % per;
        const float fade = t < 300 ? t / 300.0f : (t > per - 300 ? (per - t) / 300.0f : 1.0f);
        const Rgb col = strncmp(facts[i], "GO-", 3) == 0 ? Rgb{255, 95, 80} : kValue;
        centred(c, 56, facts[i], FrameCanvas::scale(col, nf > 1 ? fade : 1.0f));
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
    static const char *const kMonths[] = {"JAN","FEB","MAR","APR","MAY","JUN","JUL","AUG","SEP","OCT","NOV","DEC"};
    char date[16];
    snprintf(date, sizeof(date), "%d %s", lt.tm_mday, kMonths[lt.tm_mon]);
    centred(c, 44, date, kDim);
    if (weather && weather[0]) centred(c, 55, weather, Rgb{40, 46, 58});
}

void InfoScreens::renderArrivals(FrameCanvas &c, const Arrival *rows, int n, const char *runway,
                                 const char *weather, unsigned long nowMs)
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
        c.text(6, y, a.ident, Rgb{235, 240, 245});
        c.text(54, y, a.type, kLabel);

        // Count down between fetches.
        char eta[12];
        float left = a.etaSec - (nowMs - a.dataMs) / 1000.0f;
        if (isnan(left)) snprintf(eta, sizeof(eta), "-");
        else if (left < 60) snprintf(eta, sizeof(eta), a.estimate ? "~1 MIN" : "<1 MIN");
        else snprintf(eta, sizeof(eta), "%s%d MIN", a.estimate ? "~" : "", (int)lroundf(left / 60.0f));
        c.text(FrameCanvas::W - 3 - FrameCanvas::textWidth(eta), y, eta,
               !a.estimate && left < 120 ? kValue : Rgb{200, 205, 215});
    }

    // Weather along the bottom: what the arrivals are landing into.
    if (weather && weather[0])
    {
        for (int x = 8; x < FrameCanvas::W - 8; x += 2) c.set(x, 54, kDim);
        centred(c, 56, weather, Rgb{120, 160, 190});
    }
}

void InfoScreens::renderWeather(FrameCanvas &c, const Metar &m, const char *runway, unsigned long nowMs)
{
    c.clear();
    char title[24];
    snprintf(title, sizeof(title), "%s WEATHER", g_airport.name);
    if (FrameCanvas::textWidth(title) > FrameCanvas::W - 4) snprintf(title, sizeof(title), "%s WEATHER", g_airport.icao);
    centred(c, 2, title, kTitle);
    for (int x = 8; x < FrameCanvas::W - 8; x += 2) c.set(x, 11, kDim);
    if (!m.valid) { centred(c, 30, "NO REPORT YET", kDim); return; }

    // Wind, large: "330/04" (or CALM / VRB), gusts underneath.
    char wind[12];
    if (m.windKt == 0)      snprintf(wind, sizeof(wind), "CALM");
    else if (m.windDir < 0) snprintf(wind, sizeof(wind), "VRB/%02d", m.windKt);
    else                    snprintf(wind, sizeof(wind), "%03d/%02d", m.windDir, m.windKt);
    c.text(4, 15, wind, kValue, 2, 11);
    c.text(4, 31, m.gustKt ? "GUSTS" : "KNOTS", kLabel);
    if (m.gustKt)
    {
        char g[8];
        snprintf(g, sizeof(g), "%d", m.gustKt);
        c.text(40, 31, g, Rgb{255, 95, 80});
    }

    // Compass on the right: ring of dots, arrow showing where the wind blows to.
    const int cx = 108, cy = 25, r = 11;
    for (int a = 0; a < 360; a += 30)
    {
        const float rad = a * (float)M_PI / 180.0f;
        c.set(cx + (int)lroundf(sinf(rad) * r), cy - (int)lroundf(cosf(rad) * r), a == 0 ? kTitle : kDim);
    }
    if (m.windDir >= 0 && m.windKt > 0)
    {
        // A gentle wobble so it reads as moving air.
        const float wob = sinf(nowMs / 400.0f) * 0.06f;
        const float to = (m.windDir + 180) * (float)M_PI / 180.0f + wob;
        const float dx = sinf(to), dy = -cosf(to);
        for (int i = -(r - 3); i <= r - 3; ++i)
            c.set(cx + (int)lroundf(dx * i), cy + (int)lroundf(dy * i), kValue);
        // Arrow head at the downwind end.
        const int hx = cx + (int)lroundf(dx * (r - 3)), hy = cy + (int)lroundf(dy * (r - 3));
        for (int k = 1; k <= 3; ++k)
            for (int side = -1; side <= 1; side += 2)
                c.set(hx - (int)lroundf(dx * k - dy * k * side * 0.8f),
                      hy - (int)lroundf(dy * k + dx * k * side * 0.8f), kValue);
    }

    // Visibility, weather, temperature, pressure.
    char row[24], vis[8] = "-";
    if (m.visText[0]) strlcpy(vis, m.visText, sizeof(vis));
    snprintf(row, sizeof(row), "VIS %s%s%s", vis, m.wx[0] ? " " : "", m.wx);
    c.text(4, 41, row, Rgb{200, 205, 215});
    row[0] = '\0';
    if (m.tempC != -99) snprintf(row, sizeof(row), "%dC", m.tempC);
    char pr[10];
    Weather::pressure(m, pr, sizeof(pr));
    if (pr[0]) snprintf(row + strlen(row), sizeof(row) - strlen(row), "%s%s", row[0] ? "  " : "", pr);
    c.text(4, 51, row, Rgb{200, 205, 215});

    // Wind on the runway in use: crosswind, and head- or tailwind.
    if (runway && runway[0] && m.windDir >= 0 && m.windKt > 0)
    {
        const int rwyDeg = atoi(runway) * 10;   // "27L" -> 270, "1R" -> 10
        int cross = 0, tail = 0;
        Weather::components(m, rwyDeg, cross, tail);
        char rw[16];
        snprintf(rw, sizeof(rw), "XW%d %s%d", cross, tail > 0 ? "TW" : "HW", tail > 0 ? tail : -tail);
        const bool notable = cross >= 20 || tail >= 5;
        c.text(FrameCanvas::W - 4 - FrameCanvas::textWidth(rw), 51, rw, notable ? Rgb{255, 95, 80} : kLabel);
        c.text(FrameCanvas::W - 4 - FrameCanvas::textWidth(runway), 41, runway, kLabel);
    }
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

void InfoScreens::renderCaption(FrameCanvas &c, const char *text)
{
    c.clear();
    centred(c, 28, text, kTitle);
}
