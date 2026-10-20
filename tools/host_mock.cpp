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
    return 0;
}
