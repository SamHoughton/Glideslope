#include "display/InfoScreens.h"
#include "display/Fonts.h"
#include "display/Font3x5.h"
#include <math.h>
#include <time.h>
#include "config/Airport.h"
#include "config/Airports.h"
#include "config/RuntimeConfig.h"
#include "display/AircraftSprites.h"
#include "display/LandingScene.h"

namespace
{
    const Rgb kTitle {235, 240, 245};
    const Rgb kLabel { 90, 150, 170};
    const Rgb kValue {255, 185,  60};
    const Rgb kDim   { 60,  66,  80};
    const Rgb kGold  {255, 200,  60};

    void centred(FrameCanvas &c, int y, const char *s, Rgb col)
    {
        c.print(Fonts::Label, (FrameCanvas::W - FrameCanvas::width(Fonts::Label, s)) / 2, y, s, col);
    }

    // 7.0 type, centred.
    void centredL(FrameCanvas &c, int y, const char *s, Rgb col, const Fonts::Font &f = Fonts::Label)
    {
        c.print(f, (FrameCanvas::W - FrameCanvas::width(f, s)) / 2, y, s, col);
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

    // Title band shared by the 7.0 screens: a dark band fading down, an
    // accent tick, the title on the left and a dim extra (clock, count) on
    // the right.
    void titleBand(FrameCanvas &c, const char *title, const char *extra, Rgb tick = Rgb{255, 176, 50})
    {
        for (int y = 0; y < 10; ++y)
        {
            const Rgb col = mixRgb(Rgb{20, 26, 42}, Rgb{6, 8, 14}, y / 10.0f);
            for (int x = 0; x < FrameCanvas::W; ++x) c.set(x, y, col);
        }
        c.fillRect(0, 1, 2, 8, tick);
        c.print(Fonts::Label, 5, 1, title, kTitle);
        if (extra && extra[0]) c.print(Fonts::Label, FrameCanvas::W - 2 - FrameCanvas::width(Fonts::Label, extra), 1, extra, Rgb{150, 160, 180});
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

    // A horizontal bar brightening towards its end, its last column softened.
    void hbar(FrameCanvas &c, int x, int y, int w, int h, Rgb col)
    {
        for (int i = 0; i < w; ++i)
        {
            Rgb k = FrameCanvas::scale(col, 0.35f + 0.65f * (i + 1) / w);
            for (int j = 0; j < h; ++j)
                c.set(x + i, y + j, (i == w - 1 && (j == 0 || j == h - 1)) ? FrameCanvas::scale(k, 0.5f) : k);
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

// Today: the day's movements as two big figures, an hourly chart of the day
// (yesterday as a faint line to beat, this hour capped white), and a footer
// cycling through the busiest airline, the rarest type, go-arounds and
// against yesterday.
void InfoScreens::renderStats(FrameCanvas &c, const DailyStats &stats, uint32_t animMs, uint32_t shownMs)
{
    const float count = easeOut(shownMs / 900.0f);
    c.clear();
    struct tm lt;
    const bool haveTime = localNow(lt);
    char title[24], clock[8] = "";
    static const char *const kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    if (haveTime)
    {
        snprintf(title, sizeof(title), "Today  %s %d", kDays[lt.tm_wday], lt.tm_mday);
        snprintf(clock, sizeof(clock), "%02d:%02d", lt.tm_hour, lt.tm_min);
    }
    else
        snprintf(title, sizeof(title), "Today");
    titleBand(c, title, clock);

    // The figures: the count (counting up), what it is, and the last full
    // hour's movements under that.
    const int nowH = haveTime ? lt.tm_hour : -1;
    for (int side = 0; side < 2; ++side)
    {
        const bool arr = side == 0;
        const int x = arr ? 2 : 66;
        const Rgb col = arr ? kArr : kDep;
        char v[12];
        const int total = arr ? stats.arrivals() : stats.departures();
        snprintf(v, sizeof(v), "%d", (int)lroundf(total * count));
        c.print(Fonts::Huge, x, 13, v, col);
        snprintf(v, sizeof(v), "%d", total);   // its final width, so nothing moves as it counts
        const int lx = x + FrameCanvas::width(Fonts::Huge, v) + 3;
        c.print(Fonts::Label, lx, 14, arr ? "in" : "out", Rgb{150, 160, 180});
        const int lastH = nowH > 0 ? (arr ? stats.arrivalsInHour(nowH - 1) : stats.departuresInHour(nowH - 1)) : 0;
        if (lastH > 0)
        {
            snprintf(v, sizeof(v), "%d/h", lastH);
            if (lx + FrameCanvas::width(Fonts::Label, v) <= x + 62) c.print(Fonts::Label, lx, 23, v, FrameCanvas::scale(col, 0.55f));
        }
    }

    // Hourly chart: 24 columns, arrivals and departures side by side in
    // each, brighter towards the top, scaled to the busiest hour (today or
    // yesterday), on a baseline with the hours marked.
    constexpr int kTop = 34, kBase = 52, kX0 = 4;
    int peak = 1;
    for (int h = 0; h < 24; ++h)
        peak = max(peak, max(max(stats.arrivalsInHour(h), stats.departuresInHour(h)), stats.yesterdayInHour(h) / 2));
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
            const Rgb k = FrameCanvas::scale(kArr, 0.4f + 0.6f * (i + 1) / ha);
            c.set(x, kBase - 1 - i, k); c.set(x + 1, kBase - 1 - i, FrameCanvas::scale(k, 0.7f));
        }
        for (int i = 0; i < hd; ++i)
        {
            const Rgb k = FrameCanvas::scale(kDep, 0.4f + 0.6f * (i + 1) / hd);
            c.set(x + 2, kBase - 1 - i, k); c.set(x + 3, kBase - 1 - i, FrameCanvas::scale(k, 0.7f));
        }
        // Yesterday (both ways, halved to the same scale) where the bars haven't covered it.
        const int yh = stats.yesterdayInHour(h);
        if (yh)
        {
            const int y = kBase - 1 - (yh / 2 * (kBase - kTop) + peak - 1) / peak;
            for (int dx = 0; dx < 4; ++dx)
                if (c.get(x + dx, y) == 0) c.set(x + dx, y, Rgb{55, 60, 85});
        }
        if (h == nowH)
        {
            const int top = kBase - 1 - max(ha, hd);
            for (int dx = 0; dx < 4; ++dx) c.set(x + dx, top, FrameCanvas::scale(kTitle, pulse));
        }
        if (h % 6 == 0) { c.set(x + 1, kBase, kLabel); c.set(x + 2, kBase, kLabel); }
        else            { c.set(x + 1, kBase, Rgb{35, 40, 52}); c.set(x + 2, kBase, Rgb{35, 40, 52}); }
    }

    // Footer: a chip (in the fact's colour) and the fact, one at a time,
    // each for 3 s with a fade between.
    struct Fact { char chip[6]; Rgb col; char text[24]; };
    Fact facts[4];
    int nf = 0, n = 0;
    const String airline = stats.busiestAirline(n);
    if (airline.length())
    {
        Fact &f = facts[nf++];
        strlcpy(f.chip, airline.c_str(), sizeof(f.chip));
        f.col = AircraftSprites::airlineColour(airline.c_str());
        snprintf(f.text, sizeof(f.text), "Busiest, %d today", n);
    }
    const String rare = stats.rarestType();
    if (rare.length())
    {
        Fact &f = facts[nf++];
        strlcpy(f.chip, "Rare", sizeof(f.chip));
        f.col = kGold;
        snprintf(f.text, sizeof(f.text), "%s", rare.c_str());
    }
    if (stats.goArounds() > 0)
    {
        Fact &f = facts[nf++];
        strlcpy(f.chip, "GA", sizeof(f.chip));
        f.col = Rgb{255, 95, 80};
        snprintf(f.text, sizeof(f.text), stats.goArounds() == 1 ? "%d go-around" : "%d go-arounds", stats.goArounds());
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
            snprintf(f.text, sizeof(f.text), "on yesterday");
        }
    }
    if (nf)
    {
        const uint32_t per = 3000;
        const int i = (animMs / per) % nf;
        const uint32_t t = animMs % per;
        const float fade = nf > 1 ? (t < 300 ? t / 300.0f : (t > per - 300 ? (per - t) / 300.0f : 1.0f)) : 1.0f;
        const Fact &f = facts[i];
        tile(c, 0, 55, 128, 9, Rgb{14, 18, 28});
        tile(c, 1, 55, 26, 9, FrameCanvas::scale(mixRgb(Rgb{0, 0, 0}, f.col, 0.5f), fade));
        c.print(Fonts::Label, 1 + (26 - FrameCanvas::width(Fonts::Label, f.chip)) / 2, 56, f.chip, FrameCanvas::scale(kTitle, fade));
        c.print(Fonts::Label, 31, 56, f.text, FrameCanvas::scale(mixRgb(f.col, kTitle, 0.5f), fade));
    }
}

void InfoScreens::renderAircraft(FrameCanvas &c, const FlightInfo &f, Rgb accent, float etaSec, const char *runway,
                                 uint32_t shownMs)
{
    c.clear();
    // Top: the flight, and the countdown so the landing is never out of mind.
    const char *ident = f.ident_iata.length() ? f.ident_iata.c_str() : f.ident.c_str();
    char eta[16] = "";
    if (etaSec >= 0)
    {
        const int s = (int)lroundf(etaSec);
        snprintf(eta, sizeof(eta), "%s  %d:%02d", runway && runway[0] ? runway : "ETA", s / 60, s % 60);
    }
    titleBand(c, ident, "", accent);
    if (eta[0]) c.print(Fonts::Label, FrameCanvas::W - 2 - FrameCanvas::width(Fonts::Label, eta), 1, eta, kValue);

    // The aircraft in its livery, sliding in from the left.
    bool known = false;
    const AircraftSprites::Kind kind = AircraftSprites::classify(f.aircraft_code, known);
    const AircraftSprites::Sprite &sp = AircraftSprites::get(kind);   // card size: clear of the text
    const float in = easeOut(shownMs / 600.0f);
    const int sx = (int)lroundf(-sp.w + (2 + sp.w) * in);
    AircraftSprites::draw(c, sp, sx, 13 + (19 - sp.h) / 2, AircraftSprites::liveryFor(f, accent), false, !known, false, 0,
                          AircraftSprites::Lights, shownMs);

    // Beside it: type, registration, which visit this is.
    constexpr int kX = 50;
    const char *type = f.aircraft_display_name_short.length() ? f.aircraft_display_name_short.c_str() : f.aircraft_code.c_str();
    c.setClip(kX, FrameCanvas::W - 2);
    c.print(Fonts::Label, kX, 13, type, kTitle);
    if (f.registration.length()) c.print(Fonts::Label, kX, 22, f.registration.c_str(), Rgb{150, 200, 235});
    char visit[16] = "";
    if (f.seen_days == 1) snprintf(visit, sizeof(visit), "First visit");
    else if (f.seen_days > 1)
    {
        const unsigned n = f.seen_days, t = n % 100;
        const char *suf = (t >= 11 && t <= 13) ? "th" : n % 10 == 1 ? "st" : n % 10 == 2 ? "nd" : n % 10 == 3 ? "rd" : "th";
        snprintf(visit, sizeof(visit), "%u%s visit", n, suf);
    }
    if (visit[0]) c.print(Fonts::Label, kX, 31, visit, kGold);
    c.resetClip();

    // Below: the airline, then where it has come from.
    for (int x = 4; x < FrameCanvas::W - 4; x += 2) c.set(x, 40, Rgb{40, 46, 60});
    if (f.airline_display_name_full.length())
    {
        // Whole words only: "British Airways Shuttle" -> "British Airways".
        char name[32];
        strlcpy(name, f.airline_display_name_full.c_str(), sizeof(name));
        while (FrameCanvas::width(Fonts::Label, name) > FrameCanvas::W - 6)
        {
            char *sp = strrchr(name, ' ');
            if (!sp) break;
            *sp = '\0';
        }
        c.print(Fonts::Label, 3, 43, name, FrameCanvas::scale(accent, 0.9f));
    }
    char from[40] = "";
    const char *city = Airports::city(f.origin);
    const String &code = f.origin.code_iata.length() ? f.origin.code_iata : f.origin.code_icao;
    if (city || code.length()) snprintf(from, sizeof(from), "From %s", city ? city : code.c_str());
    if (from[0]) c.print(Fonts::Label, 3, 53, from, kTitle);
}

bool InfoScreens::renderOrigin(FrameCanvas &c, const FlightInfo &f, const Metar *wx, float etaSec, const char *runway,
                               uint32_t shownMs)
{
    Airports::Place from;
    if (!Airports::find(f.origin, from)) return false;
    // The great-circle distance from there to here.
    const double rad = M_PI / 180.0;
    const double la1 = from.lat * rad, la2 = g_airport.lat * rad, dlo = (g_airport.lon - from.lon) * rad;
    const double h = sin((la2 - la1) / 2) * sin((la2 - la1) / 2) + cos(la1) * cos(la2) * sin(dlo / 2) * sin(dlo / 2);
    const float km = (float)(12742.0 * asin(sqrt(h)));
    if (km < 150) return false;

    c.clear();
    const time_t now = time(nullptr);
    Metar none;
    const Metar &m = wx ? *wx : none;
    const Sky::Look sky = Sky::atPlace(m, now, from.lat, from.lon);

    // Left: the sky there, as it is now, its temperature on it.
    constexpr int kW = 52, kGround = 56;
    for (int y = 0; y < kGround; ++y)
    {
        const Rgb col = mixRgb(sky.top, sky.horizon, (float)y / kGround);
        for (int x = 0; x < kW; ++x) c.set(x, y, col);
    }
    if (sky.night > 0.5f)
    {
        static const int8_t kStars[][2] = {{6, 6}, {18, 12}, {33, 4}, {44, 15}, {9, 22}, {40, 27}, {25, 19}, {3, 31}, {47, 37}};
        if (sky.cloud < 2)
            for (int i = 0; i < 9; ++i)
            {
                const float tw = 0.5f + 0.5f * sinf(shownMs / 400.0f + i * 1.7f);
                c.set(kStars[i][0], kStars[i][1], FrameCanvas::scale(Rgb{210, 215, 230}, 0.4f + 0.6f * tw * sky.night));
            }
        Sky::drawMoon(c, 40, 9, 3.6f, Sky::moonPhase(now), Rgb{235, 230, 200});
    }
    else if (sky.cloud < 3)
    {
        const int sy = 8 + (int)lroundf((1 - min(1.0f, max(0.0f, sky.sunElev / 50.0f))) * 30);   // lower as it sets
        const Rgb sun = sky.golden > 0.3f ? Rgb{255, 190, 110} : Rgb{255, 240, 170};
        for (int dy = -4; dy <= 4; ++dy)
            for (int dx = -4; dx <= 4; ++dx)
                if (dx * dx + dy * dy <= 14) c.set(40 + dx, sy + dy, sun);
    }
    if (sky.cloud >= 1)
    {
        const Rgb cloud = FrameCanvas::scale(Rgb{205, 210, 220}, 1.0f - 0.7f * sky.night);
        const int puffs = sky.cloud == 1 ? 1 : sky.cloud == 2 ? 2 : 4;
        for (int p = 0; p < puffs; ++p)
        {
            const int cx = 8 + p * 13 + (int)((shownMs / 300 + p * 7) % 6), cy = 12 + (p % 2) * 9;
            for (int dy = -2; dy <= 2; ++dy)
                for (int dx = -6; dx <= 6; ++dx)
                    if (dx * dx / 4 + dy * dy * 2 <= 9 && cx + dx < kW) c.set(cx + dx, cy + dy, cloud);
        }
    }
    c.setClip(0, kW - 1);
    Sky::drawWeather(c, sky, shownMs, 0, kW - 1);
    c.resetClip();
    c.fillRect(0, kGround, kW, FrameCanvas::H - kGround, Sky::shade(sky, Rgb{30, 34, 44}));
    if (m.valid && m.tempC != -99)
    {
        char t[10];
        snprintf(t, sizeof(t), "%d\xC2\xB0", m.tempC);
        c.print(Fonts::Huge, 5, 33, t, Rgb{0, 0, 0});
        c.print(Fonts::Huge, 4, 32, t, m.tempC <= 0 ? Rgb{150, 200, 255} : m.tempC >= 30 ? Rgb{255, 170, 90} : kTitle);
    }
    for (int y = 0; y < FrameCanvas::H; ++y) c.set(kW, y, Rgb{0, 0, 0});

    // Right: the place, its time and weather, the journey.
    const Rgb soft{150, 160, 180}, faint{80, 88, 106};
    constexpr int kX = 56;
    c.setClip(kX, FrameCanvas::W - 1);
    c.print(Fonts::Label, kX, 1, "From", faint);
    const Fonts::Font &big = FrameCanvas::width(Fonts::Medium, from.city) <= FrameCanvas::W - kX ? Fonts::Medium : Fonts::Label;
    c.print(big, kX, &big == &Fonts::Medium ? 10 : 12, from.city, kTitle);
    if (now > 1600000000)
    {
        const time_t there = now + Airports::utcOffsetMinutes(from, now) * 60L;
        struct tm lt;
        gmtime_r(&there, &lt);
        char line[20];
        snprintf(line, sizeof(line), "%02d:%02d there", lt.tm_hour, lt.tm_min);
        c.print(Fonts::Label, kX, 23, line, soft);
    }
    {
        char words[24] = "";
        if (m.valid) Weather::describe(m.wx, words, sizeof(words));
        if (!words[0] && m.valid)
        {
            static const char *const kCloud[] = {"Clear", "Fair", "Cloudy", "Overcast"};
            snprintf(words, sizeof(words), "%s", kCloud[min(3, (int)sky.cloud)]);
            if (sky.cloud == 0 && sky.night > 0.7f) strlcat(words, " night", sizeof(words));
        }
        if (words[0])
        {
            words[0] = (char)toupper((unsigned char)words[0]);
            c.print(Fonts::Label, kX, 32, words, soft);
        }
    }
    for (int x = kX; x < FrameCanvas::W - 2; x += 2) c.set(x, 42, Rgb{40, 46, 60});
    // Roughly how long it has been flying: the distance at a typical
    // average for the trip, plus half an hour for taxi, climb and descent.
    const int mins = (int)lroundf(km / (km > 3000 ? 830.0f : 700.0f) * 60.0f + 30.0f);
    char dur[12];
    snprintf(dur, sizeof(dur), "~%d:%02d", mins / 60, (mins / 5 * 5) % 60);
    int x = kX;
    x += c.print(Fonts::Medium, x, 45, dur, kValue) + 4;
    c.print(Fonts::Label, x, 47, "flying", faint);
    char dist[16];
    const int k = (int)lroundf(km / 10.0f) * 10;
    if (k >= 1000) snprintf(dist, sizeof(dist), "%d,%03d km", k / 1000, k % 1000);
    else           snprintf(dist, sizeof(dist), "%d km", k);
    c.print(Fonts::Label, kX, 56, dist, kTitle);
    if (etaSec >= 0)
    {
        char eta[16];
        const int s2 = (int)lroundf(etaSec);
        snprintf(eta, sizeof(eta), "%d:%02d", s2 / 60, s2 % 60);   // the countdown, kept in sight
        c.print(Fonts::Label, FrameCanvas::W - 2 - FrameCanvas::width(Fonts::Label, eta), 56, eta, kValue);
    }
    (void)runway;
    c.resetClip();
    return true;
}

// The day's busiest airlines: code and a bar in the airline's colour, growing in.
void InfoScreens::renderTopAirlines(FrameCanvas &c, const DailyStats &stats, uint32_t shownMs)
{
    c.clear();
    DailyStats::Top top[5];
    const int n = stats.topAirlines(top, 5);
    char extra[16];
    snprintf(extra, sizeof(extra), "%d seen", stats.airlineCount());
    titleBand(c, "Top airlines", extra);
    const int most = n ? max(1, top[0].n) : 1;
    for (int i = 0; i < n; ++i)
    {
        const int y = 12 + i * 10;
        const Rgb col = AircraftSprites::airlineColour(top[i].code);
        c.print(Fonts::Label, 3, y + 1, top[i].code, col);
        const float grow = easeOut((shownMs - i * 120.0f) / 600.0f);
        const int w = max(2, (int)lroundf(top[i].n * 82.0f / most * grow));
        hbar(c, 24, y + 2, w, 5, col);
        char v[8];
        snprintf(v, sizeof(v), "%d", (int)lroundf(top[i].n * grow));
        c.print(Fonts::Label, 24 + w + 3, y + 1, v, kTitle);
    }
}

// The day's commonest aircraft types, coloured by family.
void InfoScreens::renderTopTypes(FrameCanvas &c, const DailyStats &stats, uint32_t shownMs)
{
    c.clear();
    DailyStats::Top top[5];
    const int n = stats.topTypes(top, 5);
    char extra[16];
    snprintf(extra, sizeof(extra), "%d types", stats.typeCount());
    titleBand(c, "Aircraft today", extra);
    const int most = n ? max(1, top[0].n) : 1;
    for (int i = 0; i < n; ++i)
    {
        const int y = 12 + i * 10;
        const Rgb col = typeColour(top[i].code);
        char code[5];
        snprintf(code, sizeof(code), "%.4s", top[i].code);
        c.print(Fonts::Label, 3, y + 1, code, col);
        const float grow = easeOut((shownMs - i * 120.0f) / 600.0f);
        const int w = max(2, (int)lroundf(top[i].n * 74.0f / most * grow));
        hbar(c, 28, y + 2, w, 5, col);
        char v[8];
        snprintf(v, sizeof(v), "%d", (int)lroundf(top[i].n * grow));
        c.print(Fonts::Label, 28 + w + 3, y + 1, v, kTitle);
    }
}

namespace
{
    // "1:45" from seconds.
    void minSec(int sec, char *out, size_t len) { snprintf(out, len, "%d:%02d", sec / 60, sec % 60); }
    void hhmm(int minute, char *out, size_t len) { snprintf(out, len, "%02d:%02d", minute / 60, minute % 60); }

    // The spotter log's kinds: a short tag and its colour.
    const char *spotTag(uint8_t kind, Rgb &col)
    {
        switch (kind)
        {
            case DailyStats::Rare:       col = Rgb{255, 200, 60};  return "Rare";
            case DailyStats::FirstVisit: col = Rgb{90, 200, 255};  return "New";
            case DailyStats::Watchlist:  col = Rgb{255, 215, 80};  return "Watch";
            case DailyStats::Overhead:   col = Rgb{120, 220, 150}; return "Over";
            case DailyStats::GoAround:   col = Rgb{255, 110, 90};  return "G/A";
            default:                     col = Rgb{255, 60, 50};   return "Sqk";
        }
    }
}

// Runway rhythm: a landing every 1:45 (the last few gaps), today's average
// and tightest gap, and each runway's share of the arrivals.
void InfoScreens::renderRhythm(FrameCanvas &c, const DailyStats &stats, const char *runway, uint32_t shownMs)
{
    c.clear();
    titleBand(c, "Runway rhythm", runway && runway[0] ? runway : "");
    const Rgb soft{150, 160, 180}, faint{80, 88, 106};
    char v[16];
    const int recent = stats.recentGapSec();
    if (recent)
    {
        minSec(recent, v, sizeof(v));
        const int w = c.print(Fonts::Huge, 2, 13, v, kArr);
        c.print(Fonts::Label, w + 6, 14, "a landing", soft);
        c.print(Fonts::Label, w + 6, 23, "every", soft);
    }
    else
    {
        c.print(Fonts::Medium, 2, 16, "Quiet", Rgb{120, 130, 150});
        c.print(Fonts::Label, 40, 18, "no recent landings", faint);
    }

    // Today: the average and the tightest pair.
    int x = 2;
    if (stats.averageGapSec())
    {
        x += c.print(Fonts::Label, x, 34, "avg", faint) + 2;
        minSec(stats.averageGapSec(), v, sizeof(v));
        x += c.print(Fonts::Label, x, 34, v, kTitle) + 6;
    }
    if (stats.tightestGapSec())
    {
        x += c.print(Fonts::Label, x, 34, "tightest", faint) + 2;
        minSec(stats.tightestGapSec(), v, sizeof(v));
        x += c.print(Fonts::Label, x, 34, v, kGold) + 3;
        c.setClip(0, FrameCanvas::W - 1);
        c.print(Fonts::Label, x, 34, stats.tightestIdent(), soft);
        c.resetClip();
    }

    // Each runway's arrivals, as bars growing in.
    DailyStats::RunwayCount rw[2];
    const int n = stats.runways(rw, 2);
    const int most = n ? max(1, (int)rw[0].n) : 1;
    for (int i = 0; i < n; ++i)
    {
        const int y = 44 + i * 10;
        c.print(Fonts::Label, 2, y, rw[i].name, kLabel);
        const float grow = easeOut((shownMs - i * 150.0f) / 600.0f);
        const int w = max(2, (int)lroundf(rw[i].n * 80.0f / most * grow));
        hbar(c, 24, y + 1, w, 5, i == 0 ? kArr : FrameCanvas::scale(kArr, 0.6f));
        snprintf(v, sizeof(v), "%d", (int)lroundf(rw[i].n * grow));
        c.print(Fonts::Label, 24 + w + 3, y, v, kTitle);
    }
}

// Today's spotter log, newest first, four at a time.
void InfoScreens::renderSpots(FrameCanvas &c, const DailyStats &stats, uint32_t shownMs)
{
    c.clear();
    char extra[16];
    snprintf(extra, sizeof(extra), "%d today", stats.spotsToday());
    titleBand(c, "Spotted today", extra, Rgb{255, 200, 60});
    const int n = stats.spotCount();
    if (!n)
    {
        centred(c, 30, "Nothing yet", kDim);
        return;
    }
    const int pages = (n + 3) / 4;
    const int page = (int)((shownMs / 4000) % pages);
    for (int k = 0; k < 4; ++k)
    {
        const int i = page * 4 + k;
        if (i >= n) break;
        const DailyStats::Spot &sp = stats.spot(i);
        const int y = 12 + k * 13;
        const float in = easeOut((shownMs % 4000 - k * 90.0f) / 350.0f);   // rows slide in
        const int dx = (int)lroundf((1 - in) * 20);
        char t[8];
        hhmm(sp.minute, t, sizeof(t));
        c.print(Fonts::Label, 2 + dx, y, t, Rgb{90, 98, 116});
        Rgb col;
        const char *tag = spotTag(sp.kind, col);
        tile(c, 30 + dx, y - 1, 25, 9, FrameCanvas::scale(col, 0.28f));
        c.print(Fonts::Label, 30 + dx + (25 - FrameCanvas::width(Fonts::Label, tag)) / 2, y, tag, col);
        c.print(Fonts::Label, 59 + dx, y, sp.ident, kTitle);
        c.print(Fonts::Label, FrameCanvas::W - 2 - FrameCanvas::width(Fonts::Label, sp.what) + dx, y, sp.what,
                Rgb{150, 200, 235});
    }
    if (pages > 1)   // which page: small dots bottom right
        for (int p2 = 0; p2 < pages; ++p2)
            c.set(FrameCanvas::W - 2 - (pages - 1 - p2) * 3, 63, p2 == page ? kTitle : kDim);
}

// The day in one card, dim like the clock it takes turns with: the movements,
// the busiest hour, the tightest gap and what was spotted. After midnight,
// yesterday (only its hours are kept).
void InfoScreens::renderDaySummary(FrameCanvas &c, const DailyStats &stats, uint32_t animMs)
{
    c.clear();
    const Rgb tone{150, 162, 195}, dim{90, 98, 120}, faint{60, 66, 84};
    struct tm lt;
    const bool haveTime = localNow(lt);
    const bool yesterday = haveTime && lt.tm_hour < 6 && stats.hasYesterday();
    int total = 0, peakH = -1, peakN = 0;
    if (yesterday)
        for (int h = 0; h < 24; ++h)
        {
            total += stats.yesterdayInHour(h);
            if (stats.yesterdayInHour(h) > peakN) { peakN = stats.yesterdayInHour(h); peakH = h; }
        }
    else
    {
        total = stats.arrivals() + stats.departures();
        peakH = stats.busiestHour(peakN);
    }
    char line[40];
    char name[24];
    strlcpy(name, g_airport.name, sizeof(name));
    Fonts::titleCase(name);
    snprintf(line, sizeof(line), "%s at %s", yesterday ? "Yesterday" : "Today", name);
    centredL(c, 2, line, dim);
    snprintf(line, sizeof(line), "%d", total);
    const float in = easeOut(animMs / 900.0f);
    char shown[12];
    snprintf(shown, sizeof(shown), "%d", (int)lroundf(total * in));
    c.print(Fonts::Huge, (FrameCanvas::W - FrameCanvas::width(Fonts::Huge, line)) / 2, 13, shown, tone);
    centredL(c, 32, "movements", dim);
    for (int x = 40; x < 88; ++x) c.set(x, 42, Rgb{26, 30, 42});
    if (peakH >= 0)
    {
        if (!yesterday && stats.tightestGapSec())
        {
            char g[8];
            minSec(stats.tightestGapSec(), g, sizeof(g));
            snprintf(line, sizeof(line), "Busiest %02d:00  tightest %s", peakH, g);
        }
        else
            snprintf(line, sizeof(line), "Busiest hour %02d:00 (%d)", peakH, peakN);
        centredL(c, 45, line, faint);
    }
    if (!yesterday && stats.spotCount())
    {
        // The rarest thing seen: a rare spot first, else the latest sighting.
        int pick = 0;
        for (int i = 0; i < stats.spotCount(); ++i)
            if (stats.spot(i).kind == DailyStats::Rare) { pick = i; break; }
        const DailyStats::Spot &sp = stats.spot(pick);
        Rgb col;
        const char *tag = spotTag(sp.kind, col);
        snprintf(line, sizeof(line), "%d spotted  %s %s", stats.spotsToday(), tag, sp.what[0] ? sp.what : sp.ident);
        centredL(c, 55, line, FrameCanvas::scale(col, 0.55f));
    }
}

void InfoScreens::renderClock(FrameCanvas &c, uint32_t animMs, const char *weather)
{
    c.clear();
    struct tm lt;
    const Rgb digits{150, 162, 195};
    if (!localNow(lt))
    {
        centredL(c, 22, "--:--", FrameCanvas::scale(digits, 0.4f), Fonts::Huge);
        return;
    }
    // HH:MM in the large soft numerals; the colon breathes with the seconds.
    char hh[3], mm[3];
    snprintf(hh, sizeof(hh), "%02d", lt.tm_hour);
    snprintf(mm, sizeof(mm), "%02d", lt.tm_min);
    const int wh = FrameCanvas::width(Fonts::Huge, hh), wc = FrameCanvas::width(Fonts::Huge, ":"), wm = FrameCanvas::width(Fonts::Huge, mm);
    const int x0 = (FrameCanvas::W - (wh + wc + wm + 2)) / 2, y = 14;
    c.print(Fonts::Huge, x0, y, hh, digits);
    const float pulse = 0.35f + 0.65f * (0.5f + 0.5f * cosf((animMs % 2000) * (float)M_PI / 1000.0f));
    c.print(Fonts::Huge, x0 + wh + 1, y, ":", FrameCanvas::scale(digits, pulse));
    c.print(Fonts::Huge, x0 + wh + wc + 2, y, mm, digits);
    // Tonight's moon, top right, in its real phase.
    Sky::drawMoon(c, FrameCanvas::W - 9, 7, 4.2f, Sky::moonPhase(time(nullptr)), Rgb{150, 150, 130});
    static const char *const kDays[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    static const char *const kMonths[] = {"January", "February", "March", "April", "May", "June", "July",
                                          "August", "September", "October", "November", "December"};
    char date[32];
    snprintf(date, sizeof(date), "%s %d %s", kDays[lt.tm_wday], lt.tm_mday, kMonths[lt.tm_mon]);
    centredL(c, 38, date, FrameCanvas::scale(digits, 0.55f));
    if (weather && weather[0])
    {
        for (int x = 40; x < 88; ++x) c.set(x, 49, Rgb{26, 30, 42});
        centredL(c, 53, weather, Rgb{60, 68, 86});
    }
}

void InfoScreens::renderNightScene(FrameCanvas &c, uint32_t animMs, const char *weather, const Sky::Look &sky,
                                   bool decorativePlane)
{
    struct tm lt;
    if (!sky.known || !localNow(lt)) { renderClock(c, animMs, weather); return; }
    LandingScene::drawBackdrop(c, animMs, sky, false);
    const int horizon = LandingScene::kHorizonY;

    // Below the skyline: dark ground and a runway's edge lights, far off.
    for (int y = horizon + 1; y < FrameCanvas::H; ++y)
        for (int x = 0; x < FrameCanvas::W; ++x) c.set(x, y, Rgb{6, 8, 12});
    const float lights = sky.lightsOn ? 1.0f : 0.25f;
    for (int x = 3; x < FrameCanvas::W; x += 5) c.set(x, horizon + 3, FrameCanvas::scale(Rgb{200, 160, 80}, 0.55f * lights));
    for (int x = 1; x < FrameCanvas::W; x += 9) c.set(x, horizon + 5, FrameCanvas::scale(Rgb{60, 200, 120}, 0.35f * lights));

    // Now and then an aircraft far off: its lights crossing above the
    // skyline, slowly coming down (one every 100 s, 30 s across).
    const uint32_t cyc = animMs % 100000;
    if (decorativePlane && cyc < 30000)
    {
        const float u = cyc / 30000.0f;
        const uint32_t n = animMs / 100000;
        const bool leftward = n % 2 == 0;
        const int x = leftward ? (int)lroundf(132 - u * 140) : (int)lroundf(-6 + u * 140);
        const int y = 33 + (int)((n * 7) % 3) + (int)lroundf(u * 4);   // under the text, over the roofs
        c.set(x, y, Rgb{255, 250, 225});                                       // landing light
        c.set(x + (leftward ? 2 : -2), y, Rgb{90, 90, 100});                   // the airframe, just
        if (animMs % 1000 < 100) c.set(x + (leftward ? 1 : -1), y - 1, Rgb{255, 40, 30});   // beacon
        const uint32_t st = (animMs + 400) % 1300;
        if (st < 45 || (st >= 140 && st < 185)) c.set(x + (leftward ? 3 : -3), y, Rgb{255, 255, 255});   // strobe
    }

    // The time above the skyline, the colon breathing with the seconds.
    const Rgb digits = sky.night > 0.5f ? Rgb{170, 182, 215} : Rgb{235, 240, 250};
    char hh[3], mm[3];
    snprintf(hh, sizeof(hh), "%02d", lt.tm_hour);
    snprintf(mm, sizeof(mm), "%02d", lt.tm_min);
    const int wh = FrameCanvas::width(Fonts::Huge, hh), wc = FrameCanvas::width(Fonts::Huge, ":"), wm = FrameCanvas::width(Fonts::Huge, mm);
    const int x0 = (FrameCanvas::W - (wh + wc + wm + 2)) / 2, y0 = 3;
    const float pulse = 0.35f + 0.65f * (0.5f + 0.5f * cosf((animMs % 2000) * (float)M_PI / 1000.0f));
    const Rgb shadow{0, 0, 0};
    c.print(Fonts::Huge, x0 + 1, y0 + 1, hh, shadow);
    c.print(Fonts::Huge, x0 + wh + wc + 3, y0 + 1, mm, shadow);
    c.print(Fonts::Huge, x0, y0, hh, digits);
    c.print(Fonts::Huge, x0 + wh + 1, y0, ":", FrameCanvas::scale(digits, pulse));
    c.print(Fonts::Huge, x0 + wh + wc + 2, y0, mm, digits);

    // Under it, in turn: the date, the next sunrise (or sunset), the weather.
    char line[40] = "";
    const int phase = (int)((animMs / 5000) % 3);
    if (phase == 1)
    {
        bool rise = false;
        const time_t ev = Sky::nextSunEvent(time(nullptr), rise);
        if (ev)
        {
            struct tm e;
            localtime_r(&ev, &e);
            snprintf(line, sizeof(line), "%s %02d:%02d", rise ? "Sunrise" : "Sunset", e.tm_hour, e.tm_min);
        }
    }
    else if (phase == 2 && weather && weather[0])
        strlcpy(line, weather, sizeof(line));
    if (!line[0])
    {
        static const char *const kDays[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
        static const char *const kMonths[] = {"January", "February", "March", "April", "May", "June", "July",
                                              "August", "September", "October", "November", "December"};
        snprintf(line, sizeof(line), "%s %d %s", kDays[lt.tm_wday], lt.tm_mday, kMonths[lt.tm_mon]);
    }
    const int lx = (FrameCanvas::W - FrameCanvas::width(Fonts::Label, line)) / 2;
    c.print(Fonts::Label, lx + 1, 24, line, shadow);
    c.print(Fonts::Label, lx, 23, line, FrameCanvas::scale(digits, 0.6f));
}

namespace
{
    // ── The split-flap board (7.0) ──────────────────────────────────────────
    // Yellow capitals on their own flaps, like the boards in a terminal:
    // each flap is 5 x 10 with a 1 px gap and the split across its middle.
    // When the board comes up the flaps run through other letters and land
    // left to right, row by row.
    const Rgb kFlapYellow{255, 205, 40};
    const Rgb kFlapFace{30, 30, 33}, kFlapFaceLow{22, 22, 25};
    constexpr int kFlapPitch = 6, kFlapH = 10;

    void flap(FrameCanvas &c, int x, int y, char ch, Rgb ink, uint32_t shownMs, uint32_t settleMs, uint32_t seed)
    {
        for (int yy = 0; yy < kFlapH; ++yy)
            for (int xx = 0; xx < 5; ++xx) c.set(x + xx, y + yy, yy < kFlapH / 2 ? kFlapFace : kFlapFaceLow);
        bool flipping = false;
        if (shownMs < settleMs)
        {
            if (shownMs + 360 < settleMs) ch = ' ';   // not started: a blank flap
            else
            {
                static const char kFlaps[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
                uint32_t h = (shownMs / 55) * 2654435761u + seed * 40503u;
                h ^= h >> 13;
                ch = kFlaps[h % (sizeof(kFlaps) - 1)];
                flipping = true;
            }
        }
        if (ch != ' ')
        {
            const char s[2] = {(char)toupper((unsigned char)ch), 0};
            const int w = FrameCanvas::width(Fonts::Label, s);
            c.print(Fonts::Label, x + (5 - w) / 2, y + 1, s, flipping ? FrameCanvas::scale(ink, 0.55f) : ink);
            // A falling flap: the top half dark for a moment.
            if (flipping && (shownMs / 55) % 2)
                for (int yy = 0; yy < kFlapH / 2; ++yy)
                    for (int xx = 0; xx < 5; ++xx) c.set(x + xx, y + yy, kFlapFaceLow);
        }
        for (int xx = 0; xx < 5; ++xx)   // the split
            c.set(x + xx, y + kFlapH / 2, FrameCanvas::scale(FrameCanvas::unpack(c.get(x + xx, y + kFlapH / 2)), 0.3f));
    }

    // A word on `cells` flaps (blank flaps after it).
    void flapWord(FrameCanvas &c, int x, int y, const char *s, int cells, Rgb ink, uint32_t shownMs, uint32_t delayMs)
    {
        const size_t n = strlen(s);
        for (int i = 0; i < cells; ++i)
            flap(c, x + i * kFlapPitch, y, i < (int)n ? s[i] : ' ', ink, shownMs, delayMs + 160 + i * 45, (uint32_t)(i * 7 + y * 131));
    }

    // The board's header: the title in yellow, the runway and the time right.
    void boardHeader(FrameCanvas &c, const char *title, const char *runway)
    {
        c.print(Fonts::Label, 1, 1, title, kFlapYellow);
        char right[24] = "";
        const time_t now = time(nullptr);
        if (now > 1600000000)
        {
            struct tm lt;
            localtime_r(&now, &lt);
            snprintf(right, sizeof(right), "%s%s%02d:%02d", runway ? runway : "", runway && runway[0] ? "  " : "", lt.tm_hour, lt.tm_min);
        }
        else if (runway) snprintf(right, sizeof(right), "%s", runway);
        c.print(Fonts::Label, 127 - FrameCanvas::width(Fonts::Label, right), 1, right, Rgb{150, 160, 180});
    }

    // The middle column: the city when the setting is on and it fits on
    // eight flaps, without its spaces if that makes it fit; otherwise the
    // airport code (LAX reads better than LOS ANGE).
    void placeFor(const char *code, char *out, size_t len)
    {
        out[0] = 0;
        if (!code || !code[0]) return;
        const char *full = g_config.route_cities ? Airports::city(code) : nullptr;
        size_t letters = 0;
        if (full) for (const char *p = full; *p; ++p) letters += *p != ' ';
        if (!full || letters > 8 || !Airports::shortCity(code, 8, out, len)) snprintf(out, len, "%s", code);
    }

    constexpr int kRowY0 = 11, kRowPitch = 11;
    constexpr int kIdentX = 2, kPlaceX = 46, kTimeX = 97;
}

void InfoScreens::renderArrivals(FrameCanvas &c, const Arrival *rows, int n, const char *runway,
                                 const char *weather, unsigned long nowMs, uint32_t shownMs)
{
    c.clear();
    boardHeader(c, "Arrivals", runway);
    if (n == 0)
    {
        const char *none = "Nothing inbound";
        c.print(Fonts::Label, (128 - FrameCanvas::width(Fonts::Label, none)) / 2, 28, none, Rgb{90, 96, 110});
    }
    const time_t now = time(nullptr);
    for (int i = 0; i < n && i < kMaxArrivals; ++i)
    {
        const Arrival &a = rows[i];
        const int y = kRowY0 + i * kRowPitch;
        c.fillRect(0, y, 1, kFlapH, a.accent);   // airline colour tab
        flapWord(c, kIdentX, y, a.ident, 7, kFlapYellow, shownMs, i * 110);
        char place[12];
        placeFor(a.from, place, sizeof(place));
        flapWord(c, kPlaceX, y, place[0] ? place : a.type, 8, place[0] ? kFlapYellow : Rgb{200, 175, 90}, shownMs, i * 110 + 200);

        // Expected: the clock time it should land (counting down between
        // fetches), LAND in white inside a minute and a half; a rough guess
        // for aircraft not yet on final is dimmer. Without the clock, minutes.
        char eta[8];
        Rgb col = kFlapYellow;
        const float left = a.etaSec - (nowMs - a.dataMs) / 1000.0f;
        if (isnan(left)) snprintf(eta, sizeof(eta), "-");
        else if (!a.estimate && left < 90) { snprintf(eta, sizeof(eta), "LAND"); col = Rgb{255, 255, 255}; }
        else if (now > 1600000000)
        {
            const time_t at = now + (time_t)lroundf(max(0.0f, left));
            struct tm lt;
            localtime_r(&at, &lt);
            snprintf(eta, sizeof(eta), "%02d:%02d", lt.tm_hour, lt.tm_min);
        }
        else snprintf(eta, sizeof(eta), "%dMIN", max(1, (int)lroundf(left / 60.0f)));
        if (a.estimate) col = FrameCanvas::scale(col, 0.7f);
        flapWord(c, kTimeX, y, eta, 5, col, shownMs, i * 110 + 350);
    }

    // Weather along the bottom: what the arrivals are landing into.
    if (weather && weather[0]) c.print(Fonts::Label, 1, 56, weather, Rgb{90, 100, 120});
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
    boardHeader(c, "Departed", runways);
    if (n == 0)
    {
        const char *none = "None recently";
        c.print(Fonts::Label, (128 - FrameCanvas::width(Fonts::Label, none)) / 2, 28, none, Rgb{90, 96, 110});
        return;
    }
    for (int i = 0; i < n && i < kMaxDepartures; ++i)
    {
        const Departure &d = rows[i];
        const int y = kRowY0 + i * kRowPitch;
        c.fillRect(0, y, 1, kFlapH, d.accent);
        flapWord(c, kIdentX, y, d.ident, 7, kFlapYellow, shownMs, i * 110);
        char place[12];
        placeFor(d.dest, place, sizeof(place));
        flapWord(c, kPlaceX, y, place[0] ? place : d.type, 8, place[0] ? kFlapYellow : Rgb{200, 175, 90}, shownMs, i * 110 + 200);
        char when[8] = "";
        if (d.at)
        {
            struct tm lt;
            localtime_r(&d.at, &lt);
            snprintf(when, sizeof(when), "%02d:%02d", lt.tm_hour, lt.tm_min);
        }
        flapWord(c, kTimeX, y, when, 5, kFlapYellow, shownMs, i * 110 + 350);
    }
}

namespace
{
    // The initial course from the airport to an airport code (IATA or
    // ICAO), degrees true; false when the code isn't in the table.
    bool courseTo(const char *code, float &deg)
    {
        Airports::Place p;
        const size_t n = code ? strlen(code) : 0;
        if (!((n == 3 && Airports::byIata(code, p)) || (n == 4 && Airports::byIcao(code, p)))) return false;
        const double r = M_PI / 180.0;
        const double la1 = g_airport.lat * r, la2 = p.lat * r, dlo = (p.lon - g_airport.lon) * r;
        const double y = sin(dlo) * cos(la2), x = cos(la1) * sin(la2) - sin(la1) * cos(la2) * cos(dlo);
        deg = (float)fmod(atan2(y, x) / r + 360.0, 360.0);
        return true;
    }
}

int InfoScreens::departureHeadings(const Departure *rows, int n)
{
    int k = 0;
    float d;
    for (int i = 0; i < n; ++i) k += courseTo(rows[i].dest, d);
    return k;
}

void InfoScreens::renderHeadings(FrameCanvas &c, const Departure *rows, int n, uint32_t shownMs)
{
    c.clear();
    titleBand(c, "Departures", "heading for", kDep);
    const Rgb soft{150, 160, 180}, faint{80, 88, 106};
    const int cx = 30, cy = 38, r = 22;
    for (int a = 0; a < 360; a += 10)
    {
        const float t = a * (float)M_PI / 180;
        c.set(cx + (int)lroundf(sinf(t) * r), cy - (int)lroundf(cosf(t) * r), a % 90 == 0 ? soft : Rgb{36, 42, 56});
    }
    Font3x5::text(c, cx - 1, cy - r - 6, "N", soft, false);
    for (int i = -3; i <= 3; ++i) c.set(cx + i, cy, Rgb{150, 150, 160});   // the airport

    // The rays, oldest first so the newest is on top, each drawing outwards.
    int row = 0;
    for (int k = min(n, kMaxDepartures) - 1; k >= 0; --k)
    {
        float deg;
        if (!courseTo(rows[k].dest, deg)) continue;
        const float t = deg * (float)M_PI / 180;
        const float bright = 1.0f - k * 0.18f;
        const float grow = min(1.0f, max(0.0f, (shownMs - (min(n, kMaxDepartures) - 1 - k) * 200.0f) / 700.0f));
        const int len = (int)lroundf((r - 2) * grow);
        for (int i = 4; i <= len; ++i)
            c.set(cx + (int)lroundf(sinf(t) * i), cy - (int)lroundf(cosf(t) * i),
                  FrameCanvas::scale(rows[k].accent, bright * (0.35f + 0.65f * i / r)));
        if (grow >= 1) c.set(cx + (int)lroundf(sinf(t) * (r - 1)), cy - (int)lroundf(cosf(t) * (r - 1)), Rgb{255, 255, 255});
    }
    // Beside it, newest first: the flight and where it is going.
    for (int k = 0; k < min(n, kMaxDepartures) && row < 4; ++k)
    {
        float deg;
        if (!courseTo(rows[k].dest, deg)) continue;
        const int y = 13 + row++ * 12;
        c.fillRect(60, y, 1, 9, rows[k].accent);
        c.print(Fonts::Label, 63, y, rows[k].ident, kTitle);
        char place[12];
        placeFor(rows[k].dest, place, sizeof(place));
        if (strcmp(place, rows[k].dest) != 0) Fonts::titleCase(place);   // a city, not a code
        const int w = FrameCanvas::width(Fonts::Label, place);
        const char *show = w <= 30 ? place : rows[k].dest;
        c.print(Fonts::Label, FrameCanvas::W - 1 - FrameCanvas::width(Fonts::Label, show), y, show, Rgb{150, 200, 235});
    }
    (void)faint;
}

void InfoScreens::renderWeather(FrameCanvas &c, const Metar &m, const char *runway, unsigned long nowMs,
                                const Sky::Look &skyNow, uint8_t mood, const char *outlook)
{
    c.clear();
    if (!m.valid)
    {
        centred(c, 2, "Weather", kTitle);
        centred(c, 30, "No report yet", kDim);
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
        char t[10];
        snprintf(t, sizeof(t), "%d\xC2\xB0", m.tempC);
        const Rgb tc = m.tempC <= 0 ? Rgb{150, 200, 255} : m.tempC >= 25 ? Rgb{255, 150, 80} : kTitle;
        c.print(Fonts::Huge, 4, 41, t, Rgb{0, 0, 0});   // a shadow, so it reads over the sky
        c.print(Fonts::Huge, 3, 40, t, tc);
    }
    for (int y = 0; y < FrameCanvas::H; ++y) c.set(kSceneW, y, Rgb{0, 0, 0});

    // Right: the facts, in words where it helps.
    constexpr int kX = 66;
    const Rgb soft{150, 160, 180}, faint{80, 88, 106}, red{255, 95, 80};
    c.setClip(kX, FrameCanvas::W - 1);
    char name[32];
    strlcpy(name, g_airport.name, sizeof(name));
    Fonts::titleCase(name);
    c.print(Fonts::Label, kX, 1, name, kTitle);
    char words[24];
    Weather::describe(m.wx, words, sizeof(words));
    if (!words[0] && m.cavok) snprintf(words, sizeof(words), "clear");
    // In turn with the next sunrise or sunset ("Sunset in 24 min" inside the hour).
    char sun[24] = "";
    {
        bool rise = false;
        const time_t now = time(nullptr);
        const time_t ev = Sky::nextSunEvent(now, rise);
        if (ev)
        {
            const long mins = (long)((ev - now + 59) / 60);
            struct tm e;
            localtime_r(&ev, &e);
            if (mins <= 60) snprintf(sun, sizeof(sun), "%s in %ld min", rise ? "Sunrise" : "Sunset", mins);
            else            snprintf(sun, sizeof(sun), "%s %02d:%02d", rise ? "Sunrise" : "Sunset", e.tm_hour, e.tm_min);
        }
    }
    if (words[0]) words[0] = (char)toupper((unsigned char)words[0]);
    // The airfield's mood says it more plainly (8.2).
    Rgb wordsCol = soft;
    if (mood == Weather::LowVis)                                { snprintf(words, sizeof(words), "Low visibility"); wordsCol = Rgb{255, 190, 90}; }
    else if (mood == Weather::Storm || mood == Weather::StormNear) { snprintf(words, sizeof(words), mood == Weather::Storm ? "Thunderstorm" : "Thunder nearby"); wordsCol = red; }
    const bool showSun = sun[0] && (!words[0] || (nowMs / 4000) % 2 == 1);
    if (showSun)        c.print(Fonts::Label, kX, 10, sun, Rgb{255, 190, 110});
    else if (words[0])  c.print(Fonts::Label, kX, 10, words, wordsCol);

    // Wind: direction and speed large, gusts in red after them.
    int x = kX;
    char v[12];
    if (m.windKt == 0) c.print(Fonts::Medium, x, 21, "Calm", kValue);
    else
    {
        if (m.windDir < 0) snprintf(v, sizeof(v), "VRB");
        else               snprintf(v, sizeof(v), "%03d\xC2\xB0", m.windDir);
        x += c.print(Fonts::Medium, x, 21, v, kValue) + 3;
        snprintf(v, sizeof(v), "%d", m.windKt);
        x += c.print(Fonts::Medium, x, 21, v, kTitle) + 1;
        if (m.gustKt)
        {
            snprintf(v, sizeof(v), "G%d", m.gustKt);
            c.print(Fonts::Label, x, 23, v, red);
        }
        else c.print(Fonts::Label, x, 23, "kt", faint);
    }
    c.resetClip();

    // The runway in use from above with the wind across it, and the
    // components beside it.
    drawRunwayWind(c, kX + 10, 42, 9, m, runway, nowMs);
    if (runway && runway[0])
    {
        c.print(Fonts::Label, kX + 23, 33, runway, kLabel);
        if (m.windDir >= 0 && m.windKt > 0)
        {
            int cross = 0, tail = 0;
            Weather::components(m, atoi(runway) * 10, cross, tail);   // "27L" -> 270
            snprintf(v, sizeof(v), "xw %d", cross);
            c.print(Fonts::Label, kX + 23, 41, v, cross >= 20 ? red : soft);
            snprintf(v, sizeof(v), "%s %d", tail > 0 ? "tw" : "hw", tail > 0 ? tail : -tail);
            c.print(Fonts::Label, kX + 23, 49, v, tail >= 5 ? red : soft);
        }
    }

    // Bottom: visibility and pressure.
    char vis[12] = "";
    for (size_t i = 0; m.visText[i] && i + 1 < sizeof(vis); ++i) vis[i] = (char)tolower((unsigned char)m.visText[i]);
    char pr[10];
    Weather::pressure(m, pr, sizeof(pr));
    char row[28];
    snprintf(row, sizeof(row), "%s%s%s", vis, vis[0] && pr[0] ? "  " : "", pr);
    // In turn with the runway outlook (8.2), out of step with the sun line.
    if (outlook && outlook[0] && (nowMs / 4000) % 2 == 0) c.print(Fonts::Label, kX, 57, outlook, Rgb{255, 200, 60});
    else                                                    c.print(Fonts::Label, kX, 57, row, faint);
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
    snprintf(sq, sizeof(sq), "Squawk %s", code);
    centred(c, 5, sq, on ? Rgb{255, 255, 255} : red);
    const Fonts::Font &big = FrameCanvas::width(Fonts::Large, meaning) <= FrameCanvas::W - 6 ? Fonts::Large : Fonts::Medium;
    centredL(c, &big == &Fonts::Large ? 16 : 18, meaning, red, big);
    centredL(c, 35, ident, kTitle, Fonts::Medium);
    centred(c, 50, detail, Rgb{200, 205, 215});
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
    char l1[32];
    strlcpy(l1, line1, sizeof(l1));
    Fonts::sentenceCase(l1);
    centred(c, 14, l1, titleCol);
    const Fonts::Font &big = FrameCanvas::width(Fonts::Large, line2) <= FrameCanvas::W - 4 ? Fonts::Large
                           : FrameCanvas::width(Fonts::Medium, line2) <= FrameCanvas::W - 4 ? Fonts::Medium : Fonts::Label;
    centredL(c, 28, line2, kGold, big);
}

void InfoScreens::renderMoodBanner(FrameCanvas &c, uint8_t style, const char *line1, const char *line2, uint32_t tMs)
{
    c.clear();
    Rgb title{255, 255, 255}, subject{230, 235, 240};
    if (style == 1)
    {
        // Fog: soft grey banks drifting slowly to the left, thicker low down.
        const float drift = tMs / 900.0f;
        for (int y = 0; y < FrameCanvas::H; ++y)
        {
            const float row = 0.10f + 0.16f * y / FrameCanvas::H + 0.05f * sinf(y * 0.37f + drift * 0.6f);
            for (int x = 0; x < FrameCanvas::W; ++x)
            {
                const float k = row + 0.05f * sinf(x * 0.071f + drift + sinf(y * 0.21f) * 1.5f) + 0.025f * sinf(x * 0.19f - drift * 1.7f + y * 0.5f);
                const uint8_t v = (uint8_t)(255 * fmaxf(0.0f, fminf(0.45f, k)));
                c.set(x, y, Rgb{v, (uint8_t)(v + v / 30), (uint8_t)(v + v / 12)});
            }
        }
        title = Rgb{200, 208, 220};
    }
    else if (style == 2)
    {
        // Thunderstorm: dark cloud, slanting rain, now and then a flash and a bolt.
        const uint32_t cycle = tMs % 1700;
        const bool flash = cycle < 90 || (cycle > 160 && cycle < 210);
        const Rgb sky = flash ? Rgb{70, 70, 110} : Rgb{10, 10, 26};
        c.fillRect(0, 0, FrameCanvas::W, FrameCanvas::H, sky);
        for (int i = 0; i < 46; ++i)
        {
            const int x0 = (i * 37 + 11) % FrameCanvas::W, speed = 3 + i % 3;
            const int y = (int)((tMs / 16 * speed + i * 23) % (FrameCanvas::H + 8)) - 4;
            const int x = (x0 - (int)(tMs / 16 * speed) / 3 % FrameCanvas::W + FrameCanvas::W) % FrameCanvas::W;
            c.set(x, y, Rgb{60, 80, 130});
            c.set(x - 1, y + 2, Rgb{40, 55, 95});
        }
        if (cycle < 210)
        {
            // A jagged bolt, somewhere new each time.
            uint32_t seed = tMs / 1700 * 2654435761u;
            int x = 12 + (int)(seed >> 8) % (FrameCanvas::W - 24);
            for (int y = 0; y < 26; ++y)
            {
                seed = seed * 1103515245u + 12345u;
                if (y % 3 == 0) x += (int)((seed >> 16) % 3) - 1;
                c.set(x, y, Rgb{255, 255, 230});
                c.set(x + 1, y, Rgb{150, 150, 200});
            }
        }
        subject = Rgb{255, 200, 60};
    }
    else
    {
        // A runway change on the way: a yellow-on-black direction sign.
        title = Rgb{200, 205, 215};
        subject = Rgb{255, 200, 0};
        const int w = FrameCanvas::width(Fonts::Large, line2) + 14;
        const int x0 = (FrameCanvas::W - w) / 2;
        c.fillRect(x0, 25, w, 24, Rgb{255, 200, 0});
        c.fillRect(x0 + 2, 27, w - 4, 20, Rgb{0, 0, 0});
    }
    centred(c, 14, line1, title);
    const Fonts::Font &big = FrameCanvas::width(Fonts::Large, line2) <= FrameCanvas::W - 4 ? Fonts::Large
                           : FrameCanvas::width(Fonts::Medium, line2) <= FrameCanvas::W - 4 ? Fonts::Medium : Fonts::Label;
    centredL(c, 28, line2, subject, big);
}

namespace
{
    void line(FrameCanvas &c, int x0, int y0, int x1, int y1, Rgb col)
    {
        int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
        for (int guard = 0; guard < 256; ++guard)
        {
            c.set(x0, y0, col);
            if (x0 == x1 && y0 == y1) break;
            const int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    }

    // Runway course from its name ("27L" -> 270), NAN if it isn't one.
    float courseOf(const char *rwy)
    {
        if (!rwy || !isdigit((unsigned char)rwy[0]) || !isdigit((unsigned char)rwy[1])) return NAN;
        return ((rwy[0] - '0') * 10 + (rwy[1] - '0')) * 10.0f;
    }

    // One split-flap tile at double size: a dark tile with the split across
    // its middle; while flipping it shows passing letters, dimmer.
    void flapTile(FrameCanvas &c, int x, int y, char ch, Rgb col, bool flipping, uint32_t seed)
    {
        for (int yy = 0; yy < 18; ++yy)
            for (int xx = 0; xx < 14; ++xx) c.set(x + xx, y + yy, Rgb{24, 27, 36});
        char s[2] = {ch, 0};
        if (flipping)
        {
            static const char kFlaps[] = "0123456789LRC";
            s[0] = kFlaps[seed % (sizeof(kFlaps) - 1)];
        }
        c.print(Fonts::Large, x + (14 - FrameCanvas::width(Fonts::Large, s)) / 2, y + 2, s, flipping ? FrameCanvas::scale(col, 0.6f) : col);
        for (int xx = 0; xx < 14; ++xx) c.set(x + xx, y + 8, Rgb{0, 0, 0});   // the split
    }
}

// The landing direction has changed. The runway's number flips over on
// split-flap tiles, a windsock swings round to the new wind, and a little
// runway seen from above shows the end the arrivals now come in over.
void InfoScreens::renderRunwayChange(FrameCanvas &c, const char *from, const char *to, uint32_t tMs)
{
    c.clear();
    constexpr uint32_t kFlipAt = 1200, kFlipMs = 1500;
    const float u = tMs < kFlipAt ? 0 : (tMs > kFlipAt + kFlipMs ? 1 : (float)(tMs - kFlipAt) / kFlipMs);
    const float e = u * u * (3 - 2 * u);   // smoothstep
    const bool flipped = u >= 1;
    const char *shown = (from && from[0] && u < 0.5f) ? from : to;

    // Title, pulsing once the new runway is in.
    const float k = flipped ? 0.6f + 0.4f * sinf(tMs / 160.0f) : 1.0f;
    centred(c, 2, "Runway change", FrameCanvas::scale(kValue, k));

    // The tiles: three characters, flipping one after another.
    const int tilesX = 6, tilesY = 16;
    for (int i = 0; i < 3; ++i)
    {
        const char target = to[i] ? to[i] : ' ', old = from && from[i] ? from[i] : ' ';
        const uint32_t start = kFlipAt + i * 220, end = start + 700;
        const bool flipping = tMs >= start && tMs < end && target != old;
        const char ch = tMs >= end ? target : (tMs < start ? old : target);
        flapTile(c, tilesX + i * 16, tilesY, ch, tMs >= end ? kValue : kTitle, flipping,
                 (tMs / 60) * 2654435761u + i * 977u);
    }
    (void)shown;

    // A runway from above (north up), the arrivals' end marked by an
    // aircraft coming in, and a windsock blowing downwind.
    const float cOld = courseOf(from), cNew = courseOf(to);
    if (!isnan(cNew))
    {
        const int cx = 96, cy = 27;
        // The runway's axis is the same both ways round (27 and 09): drawn once.
        const float ax = sinf(cNew * (float)M_PI / 180), ay = -cosf(cNew * (float)M_PI / 180);
        for (int w = -1; w <= 1; ++w)
            line(c, (int)lroundf(cx - ax * 16 - ay * w), (int)lroundf(cy - ay * 16 + ax * w),
                 (int)lroundf(cx + ax * 16 - ay * w), (int)lroundf(cy + ay * 16 + ax * w), Rgb{70, 74, 86});
        for (int d = -12; d <= 12; d += 4)   // centreline
            c.set((int)lroundf(cx + ax * d), (int)lroundf(cy + ay * d), Rgb{200, 205, 215});

        // The aircraft: a dot with a trail, coming in over the landing end
        // (old end fading out, new end fading in).
        auto approach = [&](float course, float bright) {
            if (isnan(course) || bright <= 0.02f) return;
            const float bx = sinf(course * (float)M_PI / 180), by = -cosf(course * (float)M_PI / 180);
            const float q = (tMs % 1600) / 1600.0f;   // gliding in, again and again
            const float d = 30 - q * 12;              // from 30 px out to the threshold at 18
            const int px = (int)lroundf(cx - bx * d), py = (int)lroundf(cy - by * d);
            for (int t = 1; t <= 4; ++t)
                c.set((int)lroundf(cx - bx * (d + t * 2)), (int)lroundf(cy - by * (d + t * 2)),
                      FrameCanvas::scale(kValue, bright * (0.5f - t * 0.1f)));
            c.set(px, py, FrameCanvas::scale(Rgb{255, 255, 255}, bright));
        };
        approach(cOld, 1 - e);
        approach(cNew, e);

        // Windsock: blows the way the arrivals fly (they land into the wind),
        // swinging the short way round from the old course to the new.
        float sockFrom = isnan(cOld) ? cNew : cOld, sockTo = cNew;
        float diff = fmodf(sockTo - sockFrom + 540, 360) - 180;
        const float sock = (sockFrom + diff * e) * (float)M_PI / 180;
        const int sx = 116, sy = 10;
        c.set(sx, sy, Rgb{200, 205, 215});
        for (int i = 1; i <= 6; ++i)
        {
            const Rgb col = (i % 2) ? Rgb{255, 120, 40} : Rgb{235, 235, 240};
            c.set((int)lroundf(sx + sinf(sock) * i), (int)lroundf(sy - cosf(sock) * i), col);
        }
    }

    // Below: the change in words.
    char line1[24];
    snprintf(line1, sizeof(line1), "Now landing %s", to);
    centred(c, 41, line1, flipped ? kTitle : FrameCanvas::scale(kTitle, 0.4f + 0.6f * e));
    if (from && from[0])
    {
        char was[12];
        snprintf(was, sizeof(was), "was %s", from);
        centred(c, 53, was, Rgb{140, 146, 160});
    }
}

void InfoScreens::renderHolding(FrameCanvas &c, const HoldTracker::Row *rows, int n, uint32_t animMs)
{
    c.clear();
    int total = 0;
    for (int i = 0; i < n; ++i) total += rows[i].count;
    char title[20];
    snprintf(title, sizeof(title), "%d holding", total);
    centred(c, 1, title, kTitle);
    for (int x = 4; x < FrameCanvas::W - 4; ++x) c.set(x, 10, kDim);

    const int pitch = n > 4 ? 10 : 12;
    const int y0 = n > 4 ? 13 : 14;
    for (int i = 0; i < n && i < 5; ++i)
    {
        const HoldTracker::Row &r = rows[i];
        const int y = y0 + i * pitch;
        const bool any = r.count > 0;
        c.print(Fonts::Label, 2, y, r.name, any ? kLabel : kDim);
        char num[6];
        snprintf(num, sizeof(num), "%u", (unsigned)r.count);
        c.print(Fonts::Label, 42 - FrameCanvas::width(Fonts::Label, num), y, num, any ? kValue : kDim);

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
            snprintf(mins, sizeof(mins), r.longestMin ? "%um" : "<1m", (unsigned)r.longestMin);
            c.print(Fonts::Label, FrameCanvas::W - 2 - FrameCanvas::width(Fonts::Label, mins), y, mins, kTitle);
        }
    }
}

void InfoScreens::renderCaption(FrameCanvas &c, const char *text)
{
    c.clear();
    char t[32];
    strlcpy(t, text, sizeof(t));
    Fonts::sentenceCase(t);
    centred(c, 28, t, kTitle);
}
