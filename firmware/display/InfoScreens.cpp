#include "display/InfoScreens.h"
#include <math.h>
#include <time.h>

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
    // Westerly ops alternate the landing runway at 15:00 (27L <-> 27R).
    if (withSwap && westerly && localNow(lt) && lt.tm_hour >= 6 && lt.tm_hour < 15)
        snprintf(out, len, "%s UNTIL 15:00", runway);
    else
        snprintf(out, len, "%s %s ARR", westerly ? "WEST" : "EAST", runway);
}

void InfoScreens::renderStats(FrameCanvas &c, const DailyStats &stats, uint32_t animMs)
{
    c.clear();
    struct tm lt;
    char title[20];
    static const char *const kDays[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    if (localNow(lt)) snprintf(title, sizeof(title), "TODAY %s %d", kDays[lt.tm_wday], lt.tm_mday);
    else              snprintf(title, sizeof(title), "TODAY");
    centred(c, 2, title, kTitle);
    for (int x = 8; x < FrameCanvas::W - 8; x += 2) c.set(x, 11, kDim);

    char v[16];
    // Rows 11 px apart; go-arounds get a row once there has been one.
    const int rowY[4] = {15, 26, 37, 48};
    c.text(4, rowY[0], "ARR / DEP", kLabel);
    snprintf(v, sizeof(v), "%d / %d", stats.arrivals(), stats.departures());
    c.text(FrameCanvas::W - 4 - FrameCanvas::textWidth(v), rowY[0], v, kValue);

    int n = 0;
    const String airline = stats.busiestAirline(n);
    c.text(4, rowY[1], "BUSIEST", kLabel);
    if (airline.length()) snprintf(v, sizeof(v), "%s %d", airline.c_str(), n);
    else                  snprintf(v, sizeof(v), "-");
    c.text(FrameCanvas::W - 4 - FrameCanvas::textWidth(v), rowY[1], v, kValue);

    const String rare = stats.rarestType();
    c.text(4, rowY[2], "RAREST", kLabel);
    snprintf(v, sizeof(v), "%s", rare.length() ? rare.c_str() : "-");
    c.text(FrameCanvas::W - 4 - FrameCanvas::textWidth(v), rowY[2], v, kValue);

    if (stats.goArounds() > 0)
    {
        c.text(4, rowY[3], "GO-AROUNDS", kLabel);
        snprintf(v, sizeof(v), "%d", stats.goArounds());
        c.text(FrameCanvas::W - 4 - FrameCanvas::textWidth(v), rowY[3], v, Rgb{255, 95, 80});
    }

    // A slow light running along the bottom, so the screen is visibly alive.
    const int x = 4 + (int)((animMs / 40) % (FrameCanvas::W - 8));
    for (int i = 0; i < 6; ++i)
        c.set(x - i, 59, FrameCanvas::scale(kValue, 1.0f - i / 6.0f));
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
    char hm[6];
    const bool colon = (animMs / 1000) % 2 == 0;
    snprintf(hm, sizeof(hm), colon ? "%02d:%02d" : "%02d %02d", lt.tm_hour, lt.tm_min);
    centred(c, 16, hm, Rgb{120, 130, 150}, 3, 17);
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
