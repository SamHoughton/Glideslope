// Renders firmware screens on the desktop to PPM files, for checking a
// screen without a board and for the docs. Built and run by
// tools/host_build.py render OUT_DIR; tools/host_sheet.py turns the frames
// into an LED-style contact sheet.
#include <cstdio>
#include <string>
#include <vector>
#include <time.h>
#define private public   // the renderer fills the day's stats directly
#include "display/DailyStats.h"
#undef private
#include "config/RuntimeConfig.h"
#include "config/Airport.h"
#include "display/FrameCanvas.h"
#include "display/InfoScreens.h"
#include "display/Globe.h"
#include "utils/FrameBmp.h"
#include "display/LandingScene.h"
#if __has_include("display/OverheadScene.h")
#include "display/OverheadScene.h"
#define HAVE_OVERHEAD 1
#endif

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

static void busyDay(DailyStats &s)
{
    static const int arr[24] = {0,0,0,0,0,1,6,9,8,7,8,7,6,7,8,9,7,6,5,3,0,0,0,0};
    static const int dep[24] = {0,0,0,0,0,2,7,8,7,8,6,7,8,7,6,7,8,7,6,4,0,0,0,0};
    static const int yd[24]  = {0,0,0,0,0,3,12,16,15,14,14,15,14,13,15,16,15,14,11,8,6,3,0,0};
    time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    s._year = lt.tm_year; s._day = lt.tm_yday;
    for (int h = 0; h < 24; ++h)
    {
        s._arrHour[h] = arr[h]; s._depHour[h] = dep[h]; s._yHour[h] = yd[h];
        s._arrivals += arr[h]; s._departures += dep[h];
    }
    s._hasYesterday = true;
    s._goArounds = 1;
    const struct { const char *c; int n; } airlines[] = {{"BA", 98}, {"VS", 21}, {"EI", 14}, {"LH", 12}, {"AA", 9}, {"U2", 7}, {"QR", 5}};
    for (auto &a : airlines) s.addAirline(a.c, a.n);
    const struct { const char *t; int n; } types[] = {{"A320", 112}, {"A35K", 31}, {"B789", 29}, {"B77W", 27}, {"A388", 6}, {"B744", 1}};
    for (auto &t : types) if (auto *slot = s.typeSlot(t.t, true)) { slot->n = t.n; slot->seq = ++s._seq; }
}

int main(int argc, char **argv)
{
    g_out = argc > 1 ? argv[1] : ".";
#ifdef _WIN32
    _putenv("TZ=UTC0");   // the Windows C library has no DST rules: times show in UTC
#else
    setenv("TZ", "GMT0BST,M3.5.0/1,M10.5.0", 1);
#endif
    tzset();
    AirportPack::load();   // Heathrow, built in
    FrameCanvas c;

    static DailyStats stats;
    busyDay(stats);
    InfoScreens::renderStats(c, stats, 1500, 5000);       save(c, "stats_today");
    InfoScreens::renderTopAirlines(c, stats, 5000);       save(c, "stats_airlines");
    InfoScreens::renderTopTypes(c, stats, 5000);          save(c, "stats_types");
    InfoScreens::renderStats(c, stats, 1500, 300);        save(c, "stats_today_growing");

    const time_t evening = 1792281600 + 19 * 3600 + 48 * 60;   // 18 Oct 2026, 19:48 UTC
    Globe::Route r;
    r.valid = true;
    strcpy(r.ident, "BA178"); r.accent = Rgb{70, 130, 255};
    strcpy(r.from, "New York"); strcpy(r.to, "London");
    r.oLat = 40.64f; r.oLon = -73.78f; r.dLat = 51.47f; r.dLon = -0.45f;
    r.lat = 51.40f; r.lon = -1.60f; r.speedKts = 260;
    Globe::render(c, r, 6000, evening);                   save(c, "globe_jfk_lhr");
    Globe::render(c, r, 700, evening);                    save(c, "globe_turning");
    Globe::Route s2 = r;
    strcpy(s2.ident, "SQ322"); s2.accent = Rgb{255, 190, 40};
    strcpy(s2.from, "Singapore");
    s2.oLat = 1.36f; s2.oLon = 103.99f; s2.lat = 51.0f; s2.lon = 1.2f;
    s2.landed = true; s2.landedAt = evening - 300;
    Globe::render(c, s2, 6000, evening);                  save(c, "globe_sin_lhr_landed");
    Globe::Route s3 = r;
    strcpy(s3.ident, "EK2"); s3.accent = Rgb{220, 40, 50};
    strcpy(s3.from, "London"); strcpy(s3.to, "Dubai");
    s3.oLat = 51.47f; s3.oLon = -0.45f; s3.dLat = 25.25f; s3.dLon = 55.36f;
    s3.lat = 51.52f; s3.lon = 0.9f; s3.speedKts = 300;
    Globe::render(c, s3, 6000, evening);                  save(c, "globe_lhr_dxb");
    Globe::Route s4 = s3;
    strcpy(s4.ident, "QF1"); s4.accent = Rgb{230, 30, 40};
    strcpy(s4.from, "Sydney"); strcpy(s4.to, "London");
    s4.oLat = -33.95f; s4.oLon = 151.18f; s4.dLat = 51.47f; s4.dLon = -0.45f;
    s4.lat = 50.9f; s4.lon = 2.5f; s4.speedKts = 280;
    Globe::render(c, s4, 6000, evening);                  save(c, "globe_syd_lhr");

#ifdef HAVE_OVERHEAD
    OverheadScene::Info o;
    strcpy(o.ident, "QR8"); strcpy(o.type, "B777F"); strcpy(o.where, "TO DOHA");
    o.altFt = 3400; o.kind = AircraftSprites::B777; o.livery = AircraftSprites::Livery(Rgb{110, 30, 70});
    o.accent = Rgb{180, 60, 120}; o.rightward = true;
    OverheadScene::render(c, o, 3200, Sky::preview("day"));        save(c, "overhead_low_day");
    strcpy(o.ident, "VS3"); strcpy(o.type, "A350"); strcpy(o.where, "TO NEW YORK");
    o.altFt = 14250; o.kind = AircraftSprites::A350; o.livery = AircraftSprites::Livery(Rgb{210, 20, 40});
    o.accent = Rgb{230, 40, 60}; o.rightward = false;
    OverheadScene::render(c, o, 3000, Sky::preview("golden"));     save(c, "overhead_mid_golden");
    strcpy(o.ident, "UAE38"); strcpy(o.type, "A380"); strcpy(o.where, "TO DUBAI");
    o.altFt = 36000; o.kind = AircraftSprites::A380; o.livery = AircraftSprites::Livery(Rgb{200, 30, 40});
    o.accent = Rgb{220, 40, 50}; o.rightward = true;
    OverheadScene::render(c, o, 4200, Sky::preview("day"));        save(c, "overhead_high_contrail");
    strcpy(o.label, "WATCHLIST"); o.labelColour = Rgb{255, 215, 80};
    strcpy(o.ident, "G-CIVA"); strcpy(o.type, "B744"); strcpy(o.where, "");
    o.altFt = 2200; o.kind = AircraftSprites::B747; o.livery = AircraftSprites::Livery(Rgb{40, 60, 140});
    o.accent = Rgb{90, 140, 255}; o.rightward = false;
    OverheadScene::render(c, o, 3500, Sky::preview("night"));      save(c, "overhead_watch_night");
    {
        // The picture a watchlist alert carries.
        FrameBmp bmp(c);
        const std::string path = g_out + "/alert_picture.bmp";
        FILE *f = fopen(path.c_str(), "wb");
        char buf[700];
        size_t n, total = 0;
        while ((n = bmp.readBytes(buf, sizeof(buf))) > 0) { fwrite(buf, 1, n, f); total += n; }
        fclose(f);
        printf("%s (%u bytes, expected %u)\n", path.c_str(), (unsigned)total, (unsigned)bmp.size());
    }
#endif
    // The landing scene through the year.
    {
        const AircraftSprites::Livery ba(Rgb{28, 48, 110});
        const struct { const char *look, *name; uint32_t t; } kLooks[] = {
            {"night+fireworks", "landing_fireworks", 1500}, {"night+fireworks", "landing_fireworks2", 2300},
            {"night+fireworks", "landing_fireworks3", 3100}, {"night+christmas", "landing_christmas", 1500},
            {"day+frost", "landing_frost_day", 1500}, {"night+snow", "landing_snow_night", 1500},
        };
        for (const auto &k : kLooks)
        {
            LandingScene::render(c, k.t, AircraftSprites::getScene(AircraftSprites::A320), Rgb{70, 130, 255}, false,
                                 false, "BA117", "27L", LandingScene::Landing, Sky::preview(k.look), &ba);
            save(c, k.name);
        }
    }
    return 0;
}
