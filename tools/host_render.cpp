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
#include "display/CardRenderer.h"
#include "display/DemoLogo.h"
#include "display/HoldTracker.h"
#include "utils/Weather.h"
#include "display/MapRenderer.h"
#include "display/Traffic.h"
#include "adapters/ReadsbParser.h"
#include "config/AirlineCodes.h"
#include <fstream>
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

    // 7.1: the runway rhythm and the spotter log.
    s._gapSum = 128 * 90; s._gapN = 90; s._tightSec = 72; s._tightMin = 8 * 60 + 12;
    strcpy(s._tightId, "BAW117");
    const uint16_t gaps[8] = {101, 98, 112, 105, 96, 120, 108, 100};
    memcpy(s._recent, gaps, sizeof(gaps)); s._nRecent = 8;
    s._lastArr = (uint32_t)now - 40;
    strcpy(s._rwy[0].name, "27L"); s._rwy[0].n = 71;
    strcpy(s._rwy[1].name, "27R"); s._rwy[1].n = 26;
    s._nRwy = 2;
    s.noteSpot(DailyStats::Rare, "QR7", "A388");
    s.noteSpot(DailyStats::FirstVisit, "VS3", "G-VJAZ");
    s.noteSpot(DailyStats::Overhead, "LH915", "A20N");
    s.noteSpot(DailyStats::GoAround, "BA117", "27L");
    s.noteSpot(DailyStats::Watchlist, "G-CIVA", "B744");
    s.noteSpot(DailyStats::Overhead, "EI158", "A20N");
    s.noteSpot(DailyStats::FirstVisit, "SQ322", "9V-SMF");
    const uint16_t mins[7] = {9 * 60 + 41, 11 * 60 + 5, 14 * 60 + 22, 17 * 60 + 48, 19 * 60 + 2, 19 * 60 + 40, 19 * 60 + 56};
    for (int i = 0; i < 7; ++i) s._spots[i].minute = mins[i];
}

int main(int argc, char **argv)
{
    g_out = argc > 1 ? argv[1] : ".";
#ifdef _WIN32
    _putenv("TZ=BST-1");   // London in October: the Windows C library has no DST rules
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
    InfoScreens::renderRhythm(c, stats, "27L", 5000);     save(c, "stats_rhythm");
    InfoScreens::renderSpots(c, stats, 1500);             save(c, "stats_spots");
    InfoScreens::renderSpots(c, stats, 5500);             save(c, "stats_spots_page2");
    InfoScreens::renderDaySummary(c, stats, 5000);        save(c, "night_summary");

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
    strcpy(o.ident, "QR8"); strcpy(o.type, "B777F"); strcpy(o.where, "To Doha");
    o.altFt = 3400; o.kind = AircraftSprites::B777; o.livery = AircraftSprites::Livery(Rgb{110, 30, 70});
    o.accent = Rgb{180, 60, 120}; o.rightward = true;
    OverheadScene::render(c, o, 3200, Sky::preview("day"));        save(c, "overhead_low_day");
    strcpy(o.ident, "VS3"); strcpy(o.type, "A350"); strcpy(o.where, "To New York");
    o.altFt = 14250; o.kind = AircraftSprites::A350; o.livery = AircraftSprites::Livery(Rgb{210, 20, 40});
    o.accent = Rgb{230, 40, 60}; o.rightward = false;
    OverheadScene::render(c, o, 3000, Sky::preview("golden"));     save(c, "overhead_mid_golden");
    strcpy(o.ident, "UAE38"); strcpy(o.type, "A380"); strcpy(o.where, "To Dubai");
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
    // For the README: the fictional GS101 in Glideslope's amber.
    {
        Globe::Route g;
        g.valid = true;
        strcpy(g.ident, "GS101"); g.accent = Rgb{255, 185, 60};
        strcpy(g.from, "New York"); strcpy(g.to, "London");
        g.oLat = 40.64f; g.oLon = -73.78f; g.dLat = 51.47f; g.dLon = -0.45f;
        g.lat = 51.0f; g.lon = -5.5f; g.speedKts = 300;
        Globe::render(c, g, 6000, evening);               save(c, "docs_globe");
#ifdef HAVE_OVERHEAD
        OverheadScene::Info o;
        strcpy(o.ident, "GS202"); strcpy(o.type, "A380"); strcpy(o.where, "To Singapore");
        o.altFt = 37000; o.kind = AircraftSprites::A380; o.livery = AircraftSprites::Livery(Rgb{255, 185, 60});
        o.accent = Rgb{255, 185, 60}; o.rightward = true;
        OverheadScene::render(c, o, 3900, Sky::preview("day"));  save(c, "docs_overhead");
#endif
    }

    // The map with recorded Heathrow traffic (test/fixtures/heathrow.jsonl):
    // the last 200 s fed in every 10 s, so the trails have history.
    {
        std::vector<std::string> lines;
        std::ifstream in("test/fixtures/heathrow.jsonl");
        for (std::string l; std::getline(in, l);) lines.push_back(l);
        static TrafficTracker traffic;
        unsigned long ms = 1000;
        const size_t last = 108 < lines.size() ? 108 : lines.size() - 1;
        for (size_t i = last >= 20 ? last - 20 : 0; i <= last; ++i, ms += 10000)
        {
            StringStream ss(lines[i].c_str());
            std::vector<StateVector> states;
            ReadsbParser::parseStream(ss, "ac", 51.47, -0.4543, 30, states);
            std::vector<TrafficPoint> pts;
            for (const StateVector &sv : states)
            {
                TrafficPoint tp;
                tp.id = (uint32_t)strtoul(sv.icao24.c_str(), nullptr, 16);
                tp.lat = sv.lat; tp.lon = sv.lon; tp.heading = sv.heading;
                tp.gsKt = sv.velocity * 1.94384;
                tp.altFt = sv.baro_altitude * 3.28084;
                String cs = sv.callsign; cs.trim();
                char icao[4] = {0};
                strncpy(icao, cs.c_str(), 3);
                const char *iata = cs.length() > 3 ? airlineIata(icao) : nullptr;
                const std::string label = iata ? std::string(iata) + (cs.c_str() + 3) : std::string(cs.c_str());
                strncpy(tp.label, label.c_str(), sizeof(tp.label) - 1);
                strncpy(tp.type, sv.aircraft_type.c_str(), sizeof(tp.type) - 1);
                tp.size = AircraftSprites::sizeClass(tp.type);
                tp.colour = AircraftSprites::airlineColour(icao);
                pts.push_back(tp);
            }
            traffic.update(pts, ms);
        }
        for (int k = 0; k < 3; ++k)
        {
            MapRenderer::render(c, traffic, ms + k * 2500, NAN, NAN, "09L", "09R", 1.0f);
            char name[24];
            snprintf(name, sizeof(name), "map_spot%d", k);
            save(c, name);
        }
    }

    // The approach card: GS101, the showcase's flight (3.2 km out on 27L).
    {
        FlightInfo f;
        f.ident = "GSL101"; f.ident_icao = "GSL101"; f.ident_iata = "GS101";
        f.operator_icao = "GSL"; f.airline_display_name_full = "Glideslope";
        f.origin.code_iata = "JFK"; f.origin.code_icao = "KJFK";
        f.destination.code_iata = "LHR"; f.destination.code_icao = "EGLL";
        f.aircraft_code = "A35K"; f.aircraft_display_name_short = "A350-1000"; f.registration = "G-GSLP";
        f.airline_logo_rgb565.assign(kDemoLogo, kDemoLogo + 32 * 32);
        f.lat = 51.4649; f.lon = -0.4340 + 3.2 / (111.32 * cos(51.4649 * M_PI / 180.0));
        f.heading = 269.7; f.velocity = 148; f.baro_altitude = 1050; f.vertical_rate = -760; f.distance_km = 6;
        f.seen_days = 14; f.seen_prev_day = 2478;
        CardRenderer::Options o;
        o.animMs = 1200;
        CardRenderer::render(c, f, ApproachModel::evaluate(f), Rgb{255, 185, 60}, o);   save(c, "card_approach");
        g_config.route_cities = true;
        o.animMs = 1200;
        CardRenderer::render(c, f, ApproachModel::evaluate(f), Rgb{255, 185, 60}, o);   save(c, "card_cities");
        o.animMs = 9500;   // the registration history phase of the route line
        CardRenderer::render(c, f, ApproachModel::evaluate(f), Rgb{255, 185, 60}, o);   save(c, "card_history");
        g_config.route_cities = false;
        {
            // Short final: the status reads "Landing 27L" (descender in the g).
            FlightInfo g = f;
            g.lon = -0.4340 + 0.7 / (111.32 * cos(51.4649 * M_PI / 180.0));
            g.baro_altitude = 160; g.vertical_rate = -700; g.distance_km = 1;
            o.animMs = 361;   // the status at full brightness
            CardRenderer::render(c, g, ApproachModel::evaluate(g), Rgb{255, 185, 60}, o);   save(c, "card_landing");
        }
        {
            // 7.0 sprites: an A320neo with its beacon lit, a 737 MAX freighter.
            FlightInfo g = f;
            g.aircraft_code = "A20N"; g.aircraft_display_name_short = "A320neo";
            o.animMs = 2050;   // beacon on
            CardRenderer::render(c, g, ApproachModel::evaluate(g), Rgb{255, 185, 60}, o);   save(c, "card_neo_beacon");
            g.aircraft_code = "B38M"; g.aircraft_display_name_short = "737 MAX 8";
            g.ident = "FDX5"; g.ident_icao = "FDX5"; g.ident_iata = "FX5"; g.airline_display_name_full = "FedEx";
            g.airline_logo_rgb565.clear();
            o.animMs = 1200;
            CardRenderer::render(c, g, ApproachModel::evaluate(g), Rgb{120, 60, 200}, o);   save(c, "card_max_freighter");
        }
        o.landed = true; o.landedAt = 1792350600; o.animMs = 1200;
        CardRenderer::render(c, f, ApproachModel::evaluate(f), Rgb{255, 185, 60}, o);   save(c, "card_landed");
    }

    // Boards, weather, holding, the clock and the alerts.
    {
        Metar m;
        Weather::parse("METAR EGLL 181850Z 25014G26KT 9999 -RA BKN012 OVC030 11/09 Q1004", m);
        char wxLine[40];
        Weather::friendly(m, wxLine, sizeof(wxLine));
        InfoScreens::Arrival arr[4];
        const char *ids[] = {"BA117", "VS4", "EI158", "QR7"}, *types[] = {"A35K", "A339", "A20N", "A388"};
        for (int i = 0; i < 4; ++i)
        {
            strcpy(arr[i].ident, ids[i]); strcpy(arr[i].type, types[i]);
            static const char *const froms[] = {"JFK", "MCO", "DUB", ""};
            strcpy(arr[i].from, froms[i]);
            arr[i].etaSec = 40 + i * 95; arr[i].estimate = i == 3; arr[i].dataMs = 0;
            arr[i].accent = AircraftSprites::airlineColour(ids[i][0] == 'Q' ? "QR" : ids[i][0] == 'V' ? "VS" : ids[i][0] == 'E' ? "EI" : "BA");
        }
        g_fakeMillis = 0;
        InfoScreens::renderArrivals(c, arr, 4, "27L", wxLine, 0, 5000);   save(c, "board_arrivals");
        g_config.route_cities = true;
        InfoScreens::renderArrivals(c, arr, 4, "27L", wxLine, 0, 5000);   save(c, "board_arrivals_cities");
        InfoScreens::renderArrivals(c, arr, 4, "27L", wxLine, 0, 700);    save(c, "board_arrivals_flipping");
        g_config.route_cities = false;
        InfoScreens::Departure dep[3];
        const char *dids[] = {"BA283", "AA81", "U2 8061"}, *dd[] = {"LAX", "DFW", "NCE"};
        for (int i = 0; i < 3; ++i)
        {
            strcpy(dep[i].ident, dids[i]); strcpy(dep[i].dest, dd[i]); strcpy(dep[i].type, "A320");
            dep[i].at = 1792350900 - i * 120;
        }
        InfoScreens::renderDepartures(c, dep, 3, "27R", 5000);                                save(c, "board_departures");
        g_config.route_cities = true;
        InfoScreens::renderDepartures(c, dep, 3, "27R", 5000);                                save(c, "board_departures_cities");
        g_config.route_cities = false;
        InfoScreens::renderWeather(c, m, "27L", 2000, Sky::at(m, 1792350900));               save(c, "weather_evening_rain");
        HoldTracker::Row rows[] = {{"BNN", 4, 9}, {"LAM", 2, 5}, {"BIG", 0, 0}, {"OCK", 3, 7}};
        InfoScreens::renderHolding(c, rows, 4, 2000);                                         save(c, "holding");
        InfoScreens::renderClock(c, 2000, wxLine);                              save(c, "clock_night");
        InfoScreens::renderAlert(c, "7700", "EMERGENCY", "GS101 DEMO", "4200FT 12KM E", 600);  save(c, "alert_squawk");
        InfoScreens::renderRareBanner(c, "RARE SPOT", "A380", 900);                           save(c, "banner_rare");
        InfoScreens::renderRunwayChange(c, "27R", "09L", 600);                                save(c, "runway_change_a");
        InfoScreens::renderRunwayChange(c, "27R", "09L", 1900);                               save(c, "runway_change_b");
        InfoScreens::renderRunwayChange(c, "27R", "09L", 2600);                               save(c, "runway_change_c");
        InfoScreens::renderRunwayChange(c, "27R", "09L", 4500);                               save(c, "runway_change");
    }

    // On final: the aircraft page between the card's turns.
    {
        FlightInfo f;
        f.ident_iata = "BA178"; f.ident = "BAW178";
        f.aircraft_code = "A35K"; f.aircraft_display_name_short = "A350-1000";
        f.registration = "G-XWBA"; f.airline_display_name_full = "British Airways";
        f.origin.code_icao = "KJFK"; f.origin.code_iata = "JFK";
        f.destination.code_icao = "EGLL"; f.destination.code_iata = "LHR";
        f.seen_days = 14;
        InfoScreens::renderAircraft(c, f, AircraftSprites::airlineColour("BA"), 95, "27L", 2000);  save(c, "final_aircraft");
        f.seen_days = 1; f.aircraft_code = "A388"; f.aircraft_display_name_short = "A380-800";
        f.ident_iata = "EK1"; f.ident = "UAE1"; f.registration = "A6-EVB"; f.airline_display_name_full = "Emirates";
        f.origin.code_icao = "OMDB"; f.origin.code_iata = "DXB";
        InfoScreens::renderAircraft(c, f, AircraftSprites::airlineColour("EK"), 61, "27L", 2000);  save(c, "final_aircraft2");
        // For the README: the showcase's GS101.
        f.ident_iata = "GS101"; f.ident = "GSL101"; f.aircraft_code = "A35K"; f.aircraft_display_name_short = "A350-1000";
        f.registration = "G-GSLP"; f.airline_display_name_full = "Glideslope"; f.seen_days = 14;
        f.origin.code_icao = "KJFK"; f.origin.code_iata = "JFK";
        InfoScreens::renderAircraft(c, f, Rgb{255, 185, 60}, 95, "27L", 2000);                    save(c, "docs_final_aircraft");
    }

    // The landing scene through the year.
    {
        const AircraftSprites::Livery ba(Rgb{28, 48, 110});
        const struct { const char *look, *name; uint32_t t; } kLooks[] = {
            {"night+fireworks", "landing_fireworks", 1500}, {"night+fireworks", "landing_fireworks2", 2300},
            {"night+fireworks", "landing_fireworks3", 3100}, {"night+christmas", "landing_christmas", 1500},
            {"day+frost", "landing_frost_day", 1500}, {"night+snow", "landing_snow_night", 1500},
            {"night+rain", "wx_night_rain_approach", 900}, {"night+fog", "wx_night_fog_approach", 900},
            {"day+rain", "wx_day_rain_rollout", 2300},
            {"day+windy", "wx_crosswind_approach", 800}, {"day+windy", "wx_crosswind_touchdown", 1500},
        };
        for (const auto &k : kLooks)
        {
            LandingScene::render(c, k.t, AircraftSprites::getScene(AircraftSprites::A320), Rgb{70, 130, 255}, false,
                                 false, "BA117", "27L", LandingScene::Landing, Sky::preview(k.look), &ba);
            save(c, k.name);
        }
        // Take-off: the gear tucking up, and the roll on a wet runway.
        LandingScene::render(c, 2900, AircraftSprites::getScene(AircraftSprites::A320), Rgb{70, 130, 255}, false,
                             false, "BA117", "27R", LandingScene::Takeoff, Sky::preview("day"), &ba);
        save(c, "wx_takeoff_gear_up");
        LandingScene::render(c, 1100, AircraftSprites::getScene(AircraftSprites::A320), Rgb{70, 130, 255}, false,
                             false, "BA117", "27R", LandingScene::Takeoff, Sky::preview("day+rain"), &ba);
        save(c, "wx_takeoff_wet_roll");
    }
    return 0;
}
