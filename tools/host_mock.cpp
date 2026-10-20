// 7.0 design mock-ups: the redesigned screens drawn with the firmware's own
// canvas, fonts and sprites, to decide the look before the real renderers
// change. Built and run by tools/host_build.py mock OUT_DIR.
#include <cstdio>
#include <cstring>
#include <string>
#include <cmath>
#include "display/FrameCanvas.h"
#include "display/Fonts.h"
#include "display/AircraftSprites.h"
#include "display/DemoLogo.h"
#include "config/RuntimeConfig.h"

RuntimeConfig g_config{};
unsigned long g_fakeMillis = 1000;
static std::string g_out;

static void save(const FrameCanvas &c, const char *name)
{
    const std::string path = g_out + "/" + name + ".ppm";
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) { perror(path.c_str()); return; }
    fprintf(f, "P6\n%d %d\n255\n", FrameCanvas::W, FrameCanvas::H);
    for (int i = 0; i < FrameCanvas::W * FrameCanvas::H; ++i)
    {
        const Rgb p = FrameCanvas::unpack(c.pixels()[i]);
        fputc(p.r, f); fputc(p.g, f); fputc(p.b, f);
    }
    fclose(f);
    printf("%s\n", path.c_str());
}

// ── The design system ──────────────────────────────────────────────────────
namespace Ds
{
    const Rgb ink{235, 238, 245};        // primary text
    const Rgb soft{150, 160, 180};       // secondary text
    const Rgb faint{70, 78, 96};         // rules, units
    const Rgb panel{14, 18, 28};         // raised panels
    const Rgb amber{255, 176, 40};       // arrivals
    const Rgb cyan{60, 200, 255};        // departures
    const Rgb green{90, 220, 140};
    const Rgb red{255, 80, 70};

    void hline(FrameCanvas &c, int x0, int x1, int y, Rgb col) { for (int x = x0; x <= x1; ++x) c.set(x, y, col); }
    void vline(FrameCanvas &c, int x, int y0, int y1, Rgb col) { for (int y = y0; y <= y1; ++y) c.set(x, y, col); }

    // A panel with softened corners.
    void pill(FrameCanvas &c, int x, int y, int w, int h, Rgb col)
    {
        c.fillRect(x, y, w, h, col);
        const Rgb half = FrameCanvas::scale(col, 0.45f);
        for (int k = 0; k < 4; ++k)
        {
            const int px = (k & 1) ? x + w - 1 : x, py = (k & 2) ? y + h - 1 : y;
            c.set(px, py, half);
        }
    }

    // The title band shared by every screen: an accent tick, a title, and
    // something on the right (time, count).
    void title(FrameCanvas &c, const char *t, const char *right, Rgb accent)
    {
        for (int x = 0; x < 128; ++x)
            for (int y = 0; y < 10; ++y)
                c.set(x, y, FrameCanvas::scale(Rgb{20, 26, 40}, 1.0f - y * 0.06f));
        c.fillRect(0, 1, 2, 8, accent);
        c.print(Fonts::Label, 5, 1, t, ink);
        if (right) c.print(Fonts::Label, 127 - FrameCanvas::width(Fonts::Label, right), 1, right, soft);
    }

    void logo(FrameCanvas &c, int x, int y)
    {
        for (int i = 0; i < 32 * 32; ++i) c.set(x + i % 32, y + i / 32, kDemoLogo[i]);
    }
}

// ── The approach card ──────────────────────────────────────────────────────
static void card(FrameCanvas &c, bool cities, int progress)
{
    using namespace Ds;
    c.clear();
    const Rgb accent{255, 190, 40};
    logo(c, 0, 0);
    // Flight number large, the airline's name small beneath.
    c.print(Fonts::Large, 36, 1, "GS101", ink);
    c.print(Fonts::Label, 36, 17, "Glideslope Air", soft);
    // Route: from > to, or the city.
    if (cities)
    {
        int x = 36;
        x += c.print(Fonts::Label, x, 27, "New York", ink) + 3;
        for (int i = 0; i < 4; ++i) c.set(x + i, 30, accent);
        c.set(x + 2, 29, accent); c.set(x + 2, 31, accent);
        x += 6;
        c.print(Fonts::Label, x, 27, "London", soft);
    }
    else
    {
        c.print(Fonts::Label, 36, 27, "JFK", ink);
        for (int i = 0; i < 5; ++i) c.set(52 + i, 30, accent);
        c.set(55, 29, accent); c.set(55, 31, accent);
        c.print(Fonts::Label, 60, 27, "LHR", ink);
        c.print(Fonts::Label, 80, 27, "A350-1000", soft);
    }
    // Sprite under the logo.
    AircraftSprites::Livery lv(accent);
    const auto &sp = AircraftSprites::get(AircraftSprites::A350);
    AircraftSprites::draw(c, sp, 1, 50 - sp.h, lv);
    // Readouts: altitude and speed as number + unit.
    int x = 54;
    x += c.print(Fonts::Medium, x, 38, "1,050", ink) + 1;
    c.print(Fonts::Label, x, 40, "ft", faint);
    x = 96;
    x += c.print(Fonts::Medium, x, 38, "148", ink) + 1;
    c.print(Fonts::Label, x, 40, "kt", faint);
    // Bottom bar: ETA and the glide path.
    pill(c, 0, 53, 128, 11, panel);
    c.print(Fonts::Label, 3, 55, "0:42", amber);
    c.print(Fonts::Label, 23, 55, "27L", soft);
    // Glide path from (40,55) to the runway (100..126, y61).
    for (int xx = 40; xx < 100; xx += 2) c.set(xx, 55 + (xx - 40) * 6 / 60, faint);
    hline(c, 100, 126, 61, Rgb{120, 128, 140});
    for (int xx = 102; xx < 126; xx += 4) c.set(xx, 61, ink);
    const int px = 40 + progress * 60 / 100, py = 55 + (px - 40) * 6 / 60;
    c.set(px, py, ink); c.set(px - 1, py, ink); c.set(px + 1, py, ink); c.set(px - 2, py - 1, accent); c.set(px, py + 1, soft);
}

// ── Stats ──────────────────────────────────────────────────────────────────
static void stats(FrameCanvas &c)
{
    using namespace Ds;
    c.clear();
    title(c, "Today", "Sun 18 Oct", amber);
    // Two big figures.
    c.print(Fonts::Huge, 2, 13, "97", amber);
    c.print(Fonts::Label, 30, 14, "in", soft);
    c.print(Fonts::Label, 30, 24, "3/h", faint);
    c.print(Fonts::Huge, 66, 13, "98", cyan);
    c.print(Fonts::Label, 94, 14, "out", soft);
    c.print(Fonts::Label, 94, 24, "4/h", faint);
    // The day's hours: thin stacked columns on a baseline, the hour marks under.
    static const int arr[24] = {0,0,0,0,0,1,6,9,8,7,8,7,6,7,8,9,7,6,5,3,0,0,0,0};
    static const int dep[24] = {0,0,0,0,0,2,7,8,7,8,6,7,8,7,6,7,8,7,6,4,0,0,0,0};
    const int base = 54;
    for (int h = 0; h < 24; ++h)
    {
        const int x = 4 + h * 5;
        const int a = arr[h], d = dep[h];
        for (int k = 0; k < a * 2; ++k) { c.set(x, base - k, amber); c.set(x + 1, base - k, FrameCanvas::scale(amber, 0.6f)); }
        for (int k = 0; k < d * 2; ++k) { c.set(x + 2, base - k, cyan); c.set(x + 3, base - k, FrameCanvas::scale(cyan, 0.6f)); }
        if (h == 20) c.set(x + 1, base - 1, ink);   // now
    }
    hline(c, 2, 125, base + 1, faint);
    c.print(Fonts::Label, 2, 57, "06", faint);
    c.print(Fonts::Label, 57, 57, "12", faint);
    c.print(Fonts::Label, 87, 57, "18", faint);
    c.print(Fonts::Label, 104, 57, "now", soft);
}

// ── The split-flap board ───────────────────────────────────────────────────
namespace Flap
{
    const Rgb face{30, 30, 32}, faceLow{22, 22, 24}, gap{0, 0, 0}, yellow{255, 205, 40};

    // One character on its own flap: 6 x 10 (5 face + 1 gap), the split
    // across the middle. Returns the pitch.
    void tile(FrameCanvas &c, int x, int y, char ch, Rgb ink = yellow)
    {
        for (int yy = 0; yy < 10; ++yy)
            for (int xx = 0; xx < 5; ++xx)
                c.set(x + xx, y + yy, yy < 5 ? face : faceLow);
        if (ch != ' ')
        {
            char s[2] = {ch, 0};
            const int w = FrameCanvas::width(Fonts::Label, s);
            c.print(Fonts::Label, x + (5 - w) / 2, y + 1, s, ink);
        }
        for (int xx = 0; xx < 5; ++xx)   // the split
            c.set(x + xx, y + 5, FrameCanvas::scale(FrameCanvas::unpack(c.get(x + xx, y + 5)), 0.35f));
    }

    void word(FrameCanvas &c, int x, int y, const char *s, int cells, Rgb ink = yellow)
    {
        for (int i = 0; i < cells; ++i)
        {
            const char ch = (i < (int)strlen(s)) ? s[i] : ' ';
            tile(c, x + i * 6, y, ch, ink);
        }
    }
}

static void board(FrameCanvas &c, bool departures)
{
    using namespace Ds;
    c.clear();
    // Header: plain, so the flaps are the event.
    c.print(Fonts::Label, 1, 1, departures ? "Departures" : "Arrivals", Flap::yellow);
    c.print(Fonts::Label, 127 - FrameCanvas::width(Fonts::Label, "27L  20:15"), 1, "27L  20:15", soft);
    struct Row { const char *f, *p, *t; Rgb tag; };
    const Row arr[] = {
        {"BA117", "NEW YORK", "20:16", Rgb{60, 110, 255}},
        {"VS4", "MIAMI", "20:17", Rgb{220, 40, 50}},
        {"EI158", "DUBLIN", "20:19", Rgb{20, 200, 140}},
        {"QR7", "DOHA", "20:20", Rgb{160, 30, 90}},
    };
    const Row dep[] = {
        {"BA283", "LOS ANGE", "20:15", Rgb{60, 110, 255}},
        {"AA81", "DALLAS", "20:13", Rgb{120, 140, 170}},
        {"U28061", "NICE", "20:11", Rgb{255, 120, 0}},
        {"LH915", "FRANKFRT", "20:09", Rgb{250, 190, 30}},
    };
    for (int r = 0; r < 4; ++r)
    {
        const auto &e = departures ? dep[r] : arr[r];
        const int y = 11 + r * 11;
        c.fillRect(0, y, 1, 10, e.tag);
        Flap::word(c, 2, y, e.f, 6);
        Flap::word(c, 40, y, e.p, 8);
        Flap::word(c, 92, y, e.t, 5, r == 0 && !departures ? Rgb{255, 255, 255} : Flap::yellow);
    }
    c.print(Fonts::Label, 1, 56, "250/14G26  10km  rain  11", faint);
    c.print(Fonts::Label, 1 + FrameCanvas::width(Fonts::Label, "250/14G26  10km  rain  11"), 56, "\xC2\xB0", faint);
}

// ── Weather ────────────────────────────────────────────────────────────────
static void weather(FrameCanvas &c)
{
    using namespace Ds;
    c.clear();
    // Sky on the left: an evening gradient with rain.
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 52; ++x)
            c.set(x, y, Rgb{(uint8_t)(18 + y / 3), (uint8_t)(22 + y / 4), (uint8_t)(48 + y / 2)});
    for (int k = 0; k < 40; ++k)
    {
        const int x = (k * 37) % 52, y = (k * 23) % 56;
        c.set(x, y, Rgb{110, 130, 170}); c.set(x - 1, y + 2, Rgb{70, 90, 130});
    }
    c.fillRect(0, 58, 52, 6, Rgb{18, 30, 22});
    c.print(Fonts::Huge, 4, 34, "11\xC2\xB0", ink);
    // Right: the facts, each a label over a value.
    c.print(Fonts::Label, 56, 1, "Heathrow", ink);
    c.print(Fonts::Label, 56, 11, "Light rain", soft);
    c.print(Fonts::Label, 56, 24, "WIND", faint);
    int x = 56;
    x += c.print(Fonts::Medium, x, 32, "250\xC2\xB0", amber) + 3;
    x += c.print(Fonts::Medium, x, 32, "14", ink) + 1;
    c.print(Fonts::Label, x, 34, "G26", red);
    c.print(Fonts::Label, 56, 46, "VIS", faint);
    c.print(Fonts::Label, 56, 55, "10km+", soft);
    c.print(Fonts::Label, 92, 46, "RWY", faint);
    c.print(Fonts::Label, 92, 55, "27L", soft);
    c.print(Fonts::Label, 108, 55, "xw5", faint);
}

// ── The clock ──────────────────────────────────────────────────────────────
static void clock(FrameCanvas &c)
{
    using namespace Ds;
    c.clear();
    const Rgb tone{170, 180, 210};
    const char *t = "20:15";
    const int w = FrameCanvas::width(Fonts::Huge, t, 1);
    c.print(Fonts::Huge, (128 - w) / 2, 14, t, tone, 1);
    const char *d = "Sunday 18 October";
    c.print(Fonts::Label, (128 - FrameCanvas::width(Fonts::Label, d)) / 2, 37, d, FrameCanvas::scale(tone, 0.6f));
    hline(c, 34, 93, 48, Rgb{30, 34, 46});
    const char *s = "Sunrise 07:24   11\xC2\xB0 rain";
    c.print(Fonts::Label, (128 - FrameCanvas::width(Fonts::Label, s)) / 2, 52, s, FrameCanvas::scale(tone, 0.4f));
}

// ── Type specimen ──────────────────────────────────────────────────────────
static void specimen(FrameCanvas &c)
{
    c.clear();
    c.print(Fonts::Huge, 1, 1, "20:15", Ds::ink);
    c.print(Fonts::Large, 64, 1, "BA117", Ds::amber);
    c.print(Fonts::Medium, 64, 17, "27L 148kt", Ds::cyan);
    c.print(Fonts::Label, 1, 30, "Arrivals Heathrow 27L", Ds::ink);
    c.print(Fonts::Label, 1, 41, "Qatar Airways A380-800", Ds::soft);
    c.print(Fonts::Label, 1, 52, "jumpy fog: 0123456789", Ds::green);
}


// ── 8.0: the approach line-up ──────────────────────────────────────────────
// The glide path side on: the threshold on the left, 25 km out on the right,
// every aircraft on final at its real distance and height, the gap to the
// one ahead between them.
#include "display/Font3x5.h"
namespace Lineup
{
    struct Plane { const char *id; AircraftSprites::Kind kind; Rgb fin; float km; int kt; };

    constexpr int kThrX = 8, kGroundY = 55, kTopY = 26;
    constexpr float kMaxKm = 27.0f;
    int xFor(float km) { return kThrX + (int)lroundf(km / kMaxKm * (126 - kThrX)); }
    // 3 degrees: about 172 ft a km; the view tops out at 4,300 ft.
    int yFor(float km) { return kGroundY - 1 - (int)lroundf(km * 172.0f / 4300.0f * (kGroundY - 1 - kTopY)); }
}

static void lineup(FrameCanvas &c, int variant)
{
    using namespace Ds;
    using namespace Lineup;
    c.clear();
    // Sky: a faint gradient, so the planes sit in something.
    for (int y = 10; y < kGroundY; ++y)
        for (int x = 0; x < 128; ++x)
            c.set(x, y, Rgb{(uint8_t)(6 + y / 8), (uint8_t)(9 + y / 7), (uint8_t)(18 + y / 4)});
    const Plane five[] = {
        {"BA117", AircraftSprites::A350, Rgb{40, 70, 160}, 2.4f, 142},
        {"VS4", AircraftSprites::A330, Rgb{220, 30, 50}, 8.1f, 150},
        {"EI158", AircraftSprites::A320neo, Rgb{20, 160, 130}, 13.3f, 158},
        {"QR7", AircraftSprites::A380, Rgb{120, 30, 70}, 18.9f, 165},
        {"BA286", AircraftSprites::B777, Rgb{40, 70, 160}, 24.2f, 172},
    };
    const int n = variant == 0 ? 5 : 3;
    char right[16];
    snprintf(right, sizeof(right), "%d coming", n);
    title(c, "On final 27L", right, amber);

    // The glide path, dotted, and the distance marks under it.
    for (float km = 0; km <= kMaxKm; km += 0.25f)
    {
        const int x = xFor(km), y = yFor(km);
        if (((int)(km * 4)) % 3 == 0) c.set(x, y, faint);
    }
    for (int km = 5; km <= 20; km += 5) c.set(xFor((float)km), kGroundY + 1, Rgb{60, 66, 84});   // every 5 km
    // The runway: surface, piano keys, approach lights out to 900 m.
    for (int x = 0; x <= kThrX; ++x) c.set(x, kGroundY, Rgb{90, 94, 104});
    for (int x = 0; x <= kThrX; x += 2) c.set(x, kGroundY - 1, Rgb{215, 215, 220});
    for (int x = kThrX + 2; x <= xFor(0.9f); x += 2) c.set(x, kGroundY, Rgb{255, 190, 80});
    for (int x = xFor(0.9f) + 1; x < 128; ++x) c.set(x, kGroundY, Rgb{24, 30, 26});

    // The aircraft, half size, nose at their distance on the path; the
    // flight number above in the airline's colour.
    for (int i = 0; i < n; ++i)
    {
        const Plane &p = five[i];
        const AircraftSprites::Sprite &sp = AircraftSprites::get(p.kind);
        const int nx = xFor(p.km), ny = yFor(p.km);
        const int w = (sp.w + 1) / 2, h = (sp.h + 1) / 2;
        AircraftSprites::drawHalf(c, sp, nx, ny - h + 2, AircraftSprites::Livery(p.fin));
        // Labels above, or below for every other one, so neighbours don't collide.
        const int ly = (i % 2 == 0 || ny + 4 >= kGroundY - 6) ? ny - h - 5 : ny + 5;
        Font3x5::text(c, nx + w / 2 - Font3x5::width(p.id) / 2, ly, p.id, p.fin, true);
        (void)w;
    }
    // The gaps: the time to the one ahead, on the ground between them.
    for (int i = 1; i < n; ++i)
    {
        const Plane &a = five[i - 1], &b = five[i];
        const float sec = (b.km - a.km) / (b.kt * 1.852f) * 3600.0f;
        char t[8];
        snprintf(t, sizeof(t), "%d:%02d", (int)sec / 60, (int)sec % 60);
        const int x0 = xFor(a.km) + 2, x1 = xFor(b.km) - 1;
        const int mid = (x0 + x1) / 2;
        const bool tight = sec < 60;
        const Rgb col = tight ? Rgb{255, 120, 90} : soft;
        const int tw = FrameCanvas::width(Fonts::Label, t);
        // Under the ground line, with a bracket from one aircraft to the next.
        for (int x = x0 + 1; x < x1; ++x) c.set(x, kGroundY + 2, Rgb{40, 46, 60});
        c.set(x0, kGroundY + 1, faint); c.set(x1, kGroundY + 1, faint);
        c.print(Fonts::Label, mid - tw / 2, kGroundY + 2, t, col);
        (void)x0; (void)x1;
    }

}

// ── 8.0: where it came from ────────────────────────────────────────────────
static void origin(FrameCanvas &c, bool night)
{
    using namespace Ds;
    c.clear();
    // Left: the sky over the origin right now, its temperature on it.
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 52; ++x)
            c.set(x, y, night ? Rgb{(uint8_t)(6 + y / 5), (uint8_t)(8 + y / 5), (uint8_t)(26 + y / 3)}
                              : Rgb{(uint8_t)(70 + y), (uint8_t)(130 + y), (uint8_t)(230 - y / 3)});
    if (night)
    {
        static const int st[][2] = {{6, 6}, {18, 12}, {33, 4}, {44, 15}, {9, 22}, {40, 27}, {25, 19}};
        for (auto &p : st) c.set(p[0], p[1], Rgb{200, 205, 220});
        for (int dy = -3; dy <= 3; ++dy) for (int dx = -3; dx <= 3; ++dx)
            if (dx * dx + dy * dy <= 9 && (dx + 1) * (dx + 1) + dy * dy > 6) c.set(40 + dx, 9 + dy, Rgb{235, 230, 200});
    }
    else
        for (int dy = -4; dy <= 4; ++dy) for (int dx = -4; dx <= 4; ++dx)
            if (dx * dx + dy * dy <= 14) c.set(40 + dx, 10 + dy, Rgb{255, 240, 170});
    c.fillRect(0, 56, 52, 8, night ? Rgb{14, 16, 22} : Rgb{150, 120, 70});   // sand / dark ground
    c.print(Fonts::Huge, 5, 33, night ? "27\xC2\xB0" : "31\xC2\xB0", Rgb{0, 0, 0});
    c.print(Fonts::Huge, 4, 32, night ? "27\xC2\xB0" : "31\xC2\xB0", ink);
    for (int y = 0; y < 64; ++y) c.set(52, y, Rgb{0, 0, 0});

    // Right: the place, its time and weather, the journey.
    const int x = 56;
    c.print(Fonts::Label, x, 1, "From", faint);
    c.print(Fonts::Medium, x, 10, "Dubai", ink);
    c.print(Fonts::Label, x, 23, night ? "23:42 there" : "15:42 there", soft);
    c.print(Fonts::Label, x, 32, night ? "Clear night" : "Hazy sun", soft);
    for (int xx = x; xx < 126; xx += 2) c.set(xx, 42, Rgb{40, 46, 60});
    int xx = x;
    xx += c.print(Fonts::Medium, xx, 45, "6:52", amber) + 4;
    c.print(Fonts::Label, xx, 47, "in the air", faint);
    xx = x;
    xx += c.print(Fonts::Label, xx, 56, "5,470 km", ink) + 3;
    c.print(Fonts::Label, xx, 56, "EK1", Rgb{230, 60, 60});
}

// ── 8.0: where the departures are heading ──────────────────────────────────
static void headings(FrameCanvas &c)
{
    using namespace Ds;
    c.clear();
    title(c, "Departures", "where to", cyan);
    // A compass round the airport; each recent departure a ray along its
    // course to its destination, newest brightest.
    const int cx = 30, cy = 38, r = 22;
    for (int a = 0; a < 360; a += 10)
    {
        const float t = a * (float)M_PI / 180;
        c.set(cx + (int)lroundf(sinf(t) * r), cy - (int)lroundf(cosf(t) * r), a % 90 == 0 ? soft : Rgb{36, 42, 56});
    }
    c.print(Fonts::Label, cx - 2, cy - r - 9 + 11, "", faint);
    Font3x5::text(c, cx - 1, cy - r - 6, "N", soft, false);
    // The runway in the middle.
    for (int i = -4; i <= 4; ++i) c.set(cx + i, cy, Rgb{150, 150, 160});
    struct Dep { const char *id, *to, *code; int brg; Rgb col; };
    const Dep d[] = {
        {"BA283", "Los Angeles", "LAX", 302, Rgb{80, 130, 255}},
        {"EK2", "Dubai", "DXB", 104, Rgb{230, 60, 60}},
        {"AA81", "Dallas", "DFW", 286, Rgb{170, 180, 200}},
        {"U28061", "Nice", "NCE", 152, Rgb{255, 120, 20}},
    };
    for (int k = 3; k >= 0; --k)
    {
        const float t = d[k].brg * (float)M_PI / 180;
        const float bright = 1.0f - k * 0.18f;
        for (int i = 5; i <= r - 2; ++i)
            c.set(cx + (int)lroundf(sinf(t) * i), cy - (int)lroundf(cosf(t) * i),
                  FrameCanvas::scale(d[k].col, bright * (0.35f + 0.65f * i / r)));
        const int ex = cx + (int)lroundf(sinf(t) * (r - 1)), ey = cy - (int)lroundf(cosf(t) * (r - 1));
        c.set(ex, ey, Rgb{255, 255, 255});
    }
    // The list beside it.
    for (int k = 0; k < 4; ++k)
    {
        const int y = 13 + k * 12;
        c.fillRect(60, y, 1, 9, d[k].col);
        c.print(Fonts::Label, 63, y, d[k].id, ink);
        c.print(Fonts::Label, 127 - FrameCanvas::width(Fonts::Label, d[k].code), y, d[k].code, Rgb{150, 200, 235});
        c.setClip(63, 127);
        c.print(Fonts::Label, 63, y + 0, "", soft);
        c.resetClip();
        (void)d[k].to;
    }
}

int main(int argc, char **argv)
{
    g_out = argc > 1 ? argv[1] : ".";
    FrameCanvas c;
    specimen(c);       save(c, "mock_type");
    card(c, false, 70); save(c, "mock_card");
    card(c, true, 40);  save(c, "mock_card_cities");
    stats(c);          save(c, "mock_stats");
    board(c, false);   save(c, "mock_board_arr");
    board(c, true);    save(c, "mock_board_dep");
    weather(c);        save(c, "mock_weather");
    clock(c);          save(c, "mock_clock");
    lineup(c, 0);      save(c, "mock8_lineup");
    lineup(c, 1);      save(c, "mock8_lineup_quiet");
    origin(c, true);   save(c, "mock8_origin_night");
    origin(c, false);  save(c, "mock8_origin_day");
    headings(c);       save(c, "mock8_headings");
    return 0;
}
