#include "display/LineupScreen.h"
#include "display/AircraftSprites.h"
#include "display/Font3x5.h"
#include "display/Fonts.h"
#include <math.h>
#include <algorithm>

namespace
{
    constexpr int   kThrX = 8, kGroundY = 55, kTopY = 26;
    constexpr float kMaxKm = 27.0f;
    constexpr float kFtPerKm = 172.0f;            // a 3-degree glide path
    constexpr float kTopFt = kMaxKm * kFtPerKm;   // the height at the right edge

    const Rgb kInk{235, 238, 245}, kSoft{150, 160, 180}, kFaint{80, 88, 106};

    int xFor(float km) { return kThrX + (int)lroundf(km / kMaxKm * (126 - kThrX)); }
    int yFor(float ft)
    {
        const float k = ft / kTopFt;
        return kGroundY - 1 - (int)lroundf((k < 0 ? 0 : k) * (kGroundY - 1 - kTopY));
    }

    // The title band the 7.0 screens share.
    void titleBand(FrameCanvas &c, const char *title, const char *extra, Rgb tick)
    {
        for (int y = 0; y < 10; ++y)
        {
            const float k = y / 10.0f;
            const Rgb col{(uint8_t)(20 + (6 - 20) * k), (uint8_t)(26 + (8 - 26) * k), (uint8_t)(42 + (14 - 42) * k)};
            for (int x = 0; x < FrameCanvas::W; ++x) c.set(x, y, col);
        }
        c.fillRect(0, 1, 2, 8, tick);
        c.print(Fonts::Label, 5, 1, title, kInk);
        if (extra && extra[0]) c.print(Fonts::Label, FrameCanvas::W - 2 - FrameCanvas::width(Fonts::Label, extra), 1, extra, kSoft);
    }
}

int LineupScreen::collect(const TrafficTracker &traffic, unsigned long now, Plane *out)
{
    int n = 0;
    for (const TrafficTracker::Tracked &t : traffic.aircraft())
    {
        if (t.p.finalKm10 == 255) continue;
        Plane p;
        strlcpy(p.id, t.p.label, sizeof(p.id));
        strlcpy(p.type, t.p.type, sizeof(p.type));
        strlcpy(p.rwy, t.p.rwy, sizeof(p.rwy));
        p.colour = t.p.colour;
        p.gsKt = t.p.gsKt;
        // Dead-reckoned along the approach since the report (5 s at most
        // between fetches): closer by its ground speed, lower down the slope.
        const float sec = min((now - t.posMs) / 1000.0f, 15.0f);
        const float moved = isnan(p.gsKt) ? 0 : p.gsKt * 1.852f * sec / 3600.0f;
        p.km = max(0.0f, t.p.finalKm10 / 10.0f - moved);
        p.altFt = isnan(t.p.altFt) ? p.km * kFtPerKm : max(0.0f, t.p.altFt - moved * kFtPerKm);
        if (n < kMax) out[n++] = p;
        else
        {
            // Keep the nearest kMax.
            int far = 0;
            for (int i = 1; i < n; ++i) if (out[i].km > out[far].km) far = i;
            if (p.km < out[far].km) out[far] = p;
        }
    }
    std::sort(out, out + n, [](const Plane &a, const Plane &b) { return a.km < b.km; });
    return n;
}

void LineupScreen::render(FrameCanvas &c, Plane *planes, int n, uint32_t shownMs, const Sky::Look &sky)
{
    c.clear();
    std::sort(planes, planes + n, [](const Plane &a, const Plane &b) { return a.km < b.km; });

    // The sky as it is, darkened so the aircraft and their labels stand out.
    if (sky.known)
    {
        Sky::drawSky(c, sky, shownMs, kGroundY - 1);
        uint16_t *px = c.pixels();
        for (int i = 10 * FrameCanvas::W; i < kGroundY * FrameCanvas::W; ++i)
            px[i] = FrameCanvas::pack(FrameCanvas::scale(FrameCanvas::unpack(px[i]), 0.5f));
    }
    else
        for (int y = 10; y < kGroundY; ++y)
            for (int x = 0; x < FrameCanvas::W; ++x)
                c.set(x, y, Rgb{(uint8_t)(6 + y / 8), (uint8_t)(9 + y / 7), (uint8_t)(18 + y / 4)});
    for (int y = kGroundY; y < FrameCanvas::H; ++y)
        for (int x = 0; x < FrameCanvas::W; ++x) c.set(x, y, Rgb{0, 0, 0});

    char title[16], right[16];
    const char *rwy = n && planes[0].rwy[0] ? planes[0].rwy : "";
    snprintf(title, sizeof(title), rwy[0] ? "On final %s" : "On final", rwy);
    snprintf(right, sizeof(right), n == 1 ? "1 coming" : "%d coming", n);
    titleBand(c, title, right, Rgb{255, 176, 50});

    // The glide path, dotted, with a mark every 5 km below it.
    for (float km = 0; km <= kMaxKm; km += 0.25f)
        if (((int)(km * 4)) % 3 == 0) c.set(xFor(km), yFor(km * kFtPerKm), kFaint);
    for (int km = 5; km <= 25; km += 5) c.set(xFor((float)km), kGroundY + 1, Rgb{60, 66, 84});
    // The runway: surface and piano keys, the approach lights out to 900 m.
    for (int x = 0; x <= kThrX; ++x) c.set(x, kGroundY, Rgb{90, 94, 104});
    for (int x = 0; x <= kThrX; x += 2) c.set(x, kGroundY - 1, Rgb{215, 215, 220});
    const int last = xFor(0.9f), run = (int)((shownMs / 90) % 10);   // a white light running in to the threshold
    for (int x = kThrX + 2; x <= last; x += 2)
        c.set(x, kGroundY, (last - x) / 2 == run ? Rgb{255, 255, 255} : Rgb{255, 190, 80});
    for (int x = xFor(0.9f) + 1; x < FrameCanvas::W; ++x) c.set(x, kGroundY, Rgb{24, 30, 26});

    if (n == 0)
    {
        const char *t = "Nobody on final";
        c.print(Fonts::Label, (FrameCanvas::W - FrameCanvas::width(Fonts::Label, t)) / 2, 34, t, kFaint);
        return;
    }

    // The aircraft, half size, nose at its distance and height; the flight
    // number above it, or below for every other one so neighbours don't
    // collide. One about to leave the right edge fades in.
    for (int i = n - 1; i >= 0; --i)
    {
        const Plane &p = planes[i];
        bool known = false;
        const AircraftSprites::Sprite &sp = AircraftSprites::get(AircraftSprites::classify(p.type, known));
        const int nx = xFor(p.km), ny = yFor(p.altFt);
        const int w = (sp.w + 1) / 2, h = (sp.h + 1) / 2;
        if (nx > FrameCanvas::W) continue;
        AircraftSprites::Livery lv(p.colour);
        AircraftSprites::drawHalf(c, sp, nx, ny - h + 2, lv, false, !known);
        int ly = (i % 2 == 0 || ny + 10 >= kGroundY) ? ny - h - 5 : ny + 5;
        int lx = nx + w / 2 - Font3x5::width(p.id) / 2;
        // Close behind the one ahead (under a minute): its label goes past
        // its tail instead, clear of the other's.
        if (i > 0 && nx - xFor(planes[i - 1].km) < w + 2) { lx = nx + w + 1; ly = ny - h + 1; }
        lx = min(FrameCanvas::W - 1 - Font3x5::width(p.id), lx);
        const float edge = p.km > kMaxKm - 3 ? max(0.25f, (kMaxKm - p.km) / 3) : 1.0f;
        Font3x5::text(c, lx, max(11, ly), p.id, FrameCanvas::scale(p.colour, edge), true);
    }

    // The gaps: the time from each to the one ahead, under the ground line
    // with a bracket from one to the next; red under a minute.
    const float fade = min(1.0f, shownMs / 600.0f);
    for (int i = 1; i < n; ++i)
    {
        const Plane &a = planes[i - 1], &b = planes[i];
        const float gs = isnan(b.gsKt) || b.gsKt < 60 ? 140.0f : b.gsKt;
        const int sec = (int)lroundf((b.km - a.km) / (gs * 1.852f) * 3600.0f);
        int x0 = xFor(a.km) + 2, x1 = min(FrameCanvas::W - 1, xFor(b.km) - 1);
        if (x1 - x0 < 8) { const int m = (x0 + x1) / 2; x0 = m - 4; x1 = m + 4; }   // very close: still say how close
        char t[8];
        snprintf(t, sizeof(t), "%d:%02d", sec / 60, sec % 60);
        const Rgb col = FrameCanvas::scale(sec < 60 ? Rgb{255, 120, 90} : kSoft, fade);
        for (int x = x0 + 1; x < x1; ++x) c.set(x, kGroundY + 2, FrameCanvas::scale(Rgb{40, 46, 60}, fade));
        c.set(x0, kGroundY + 1, FrameCanvas::scale(kFaint, fade));
        c.set(x1, kGroundY + 1, FrameCanvas::scale(kFaint, fade));
        const int tw = FrameCanvas::width(Fonts::Label, t);
        const int tx = max(x0, min((x0 + x1) / 2 - tw / 2, FrameCanvas::W - 1 - tw));
        c.print(Fonts::Label, tx, kGroundY + 2, t, col);
    }
}

int LineupScreen::demo(Plane *out)
{
    static const struct { const char *id, *type; Rgb col; float km, gs; } k[] = {
        {"BA117", "A35K", {60, 110, 255}, 2.4f, 142},
        {"VS4", "A339", {220, 30, 50}, 8.1f, 150},
        {"EI158", "A20N", {20, 200, 140}, 13.3f, 158},
        {"QR7", "A388", {160, 30, 90}, 18.9f, 165},
        {"BA286", "B77W", {60, 110, 255}, 24.2f, 172},
    };
    int n = 0;
    for (const auto &e : k)
    {
        Plane &p = out[n++];
        p = Plane();
        strlcpy(p.id, e.id, sizeof(p.id));
        strlcpy(p.type, e.type, sizeof(p.type));
        strlcpy(p.rwy, "27L", sizeof(p.rwy));
        p.colour = e.col; p.km = e.km; p.gsKt = e.gs;
        p.altFt = e.km * kFtPerKm + (n == 3 ? 150 : 0);   // one a little high
    }
    return n;
}
