// Unit tests for the board's logic, built for the desktop with the stand-in
// Arduino layer in test/stubs:   pio test -e native
//
// The sources under test are built as they are on the board, one file at a
// time (build_src_filter in platformio.ini); they only need a few Arduino
// types, which the stubs provide.
#include <unity.h>
#include <vector>
#include <fstream>

#include "config/RuntimeConfig.h"
RuntimeConfig g_config{};
unsigned long g_fakeMillis = 1000;

#include "config/Airport.h"
#include "config/Airports.h"
#include "utils/Spotter.h"
#include "utils/SeenLog.h"
#include "utils/HttpRequest.h"
#include "display/ApproachModel.h"
#include "display/RunwayTracker.h"
#include "display/HoldTracker.h"
#include "display/DailyStats.h"
#include "utils/Logbook.h"
#include <WiFiClient.h>
#include <LittleFS.h>
#include <algorithm>
#include "utils/Weather.h"
#include "utils/Airframes.h"
#include "adapters/ReadsbParser.h"

// ── Helpers ─────────────────────────────────────────────────────────────────
namespace
{
    constexpr double kLat = 51.4700, kLon = -0.4543;   // Heathrow reference point

    // An aircraft east/north (km) of a point, in the StateVector's SI units.
    StateVector at(const char *hex, double lat0, double lon0, double kmEast, double kmNorth,
                   double altFt, double gsKt, double track, double vrFpm)
    {
        StateVector s;
        s.icao24 = hex;
        s.callsign = String("TST") + hex;
        s.lat = lat0 + kmNorth / 111.32;
        s.lon = lon0 + kmEast / (111.32 * cos(lat0 * M_PI / 180.0));
        s.baro_altitude = altFt / 3.28084;
        s.velocity = gsKt / 1.94384;
        s.heading = track;
        s.vertical_rate = vrFpm / 196.85;
        return s;
    }

    FlightInfo flight(const StateVector &s)
    {
        FlightInfo f;
        f.lat = s.lat; f.lon = s.lon; f.heading = s.heading;
        f.baro_altitude = s.baro_altitude * 3.28084;
        f.velocity = s.velocity * 1.94384;
        f.vertical_rate = s.vertical_rate * 196.85;
        return f;
    }

    // On final for a Heathrow runway end, km out, on a 3 degree slope.
    StateVector onFinal(const char *hex, const char *rwy, double km)
    {
        for (int i = 0; i < g_airport.runwayCount; ++i)
        {
            const RunwayEnd &r = g_airport.runways[i];
            if (strcmp(r.name, rwy)) continue;
            const double back = (r.course + 180.0) * M_PI / 180.0;   // out along the approach
            const double alt = km * 1000 * tan(3 * M_PI / 180) * 3.28084 + 50;
            return at(hex, r.lat, r.lon, km * sin(back), km * cos(back), alt, 140, r.course, -700);
        }
        TEST_FAIL_MESSAGE("no such runway");
        return StateVector();
    }
}

void setUp()
{
    AirportPack::load();   // Heathrow, built in (no filesystem here)
    g_config.min_altitude_ft = -1;
}
void tearDown() {}

// ── METAR ───────────────────────────────────────────────────────────────────
void test_metar_uk()
{
    Metar m;
    TEST_ASSERT_TRUE(Weather::parse("METAR EGLL 151720Z AUTO 21012G25KT 180V250 9999 -RA SCT043 17/15 Q1019", m));
    TEST_ASSERT_EQUAL(210, m.windDir);
    TEST_ASSERT_EQUAL(12, m.windKt);
    TEST_ASSERT_EQUAL(25, m.gustKt);
    TEST_ASSERT_EQUAL(9999, m.visM);
    TEST_ASSERT_EQUAL(17, m.tempC);
    TEST_ASSERT_EQUAL(1019, m.qnh);
    TEST_ASSERT_EQUAL_STRING("EGLL", m.station);   // which airport the report is from (8.0)
    TEST_ASSERT_EQUAL(15, m.obsDay);                 // observed 151720Z (9.0)
    TEST_ASSERT_EQUAL(17, m.obsHour);
    TEST_ASSERT_EQUAL(20, m.obsMin);
    TEST_ASSERT_EQUAL_STRING("-RA", m.wx);
    char line[40];
    Weather::friendly(m, line, sizeof(line));
    TEST_ASSERT_EQUAL_STRING("17\xC2\xB0  light rain  210/12G25", line);
    Weather::friendly(m, line, sizeof(line), 16);   // too narrow for the wind
    TEST_ASSERT_EQUAL_STRING("17\xC2\xB0  light rain", line);
}

void test_metar_us()
{
    Metar m;
    TEST_ASSERT_TRUE(Weather::parse("METAR KJFK 151751Z 31008KT 10SM FEW250 M02/M10 A3012", m));
    TEST_ASSERT_EQUAL(310, m.windDir);
    TEST_ASSERT_EQUAL(-2, m.tempC);
    TEST_ASSERT_EQUAL(3012, m.altInHg100);
    TEST_ASSERT_EQUAL_STRING("10SM", m.visText);
}

void test_metar_calm_and_garbage()
{
    Metar m;
    TEST_ASSERT_TRUE(Weather::parse("METAR EGLL 151720Z 00000KT CAVOK 12/08 Q1030", m));
    TEST_ASSERT_EQUAL(0, m.windKt);
    TEST_ASSERT_TRUE(m.cavok);
    char line[40];
    Weather::friendly(m, line, sizeof(line));
    TEST_ASSERT_EQUAL_STRING("12\xC2\xB0  calm", line);
    Metar bad;
    TEST_ASSERT_FALSE(Weather::parse("", bad));
}

// ── The airfield's mood and the TAF (8.2) ──────────────────────────────────
void test_metar_mood()
{
    Metar m;
    TEST_ASSERT_TRUE(Weather::parse("METAR EGLL 240620Z 00000KT 0300 FG VV001 06/06 Q1025", m));
    TEST_ASSERT_EQUAL(300, m.visM);
    TEST_ASSERT_EQUAL(100, m.ceilingFt);
    TEST_ASSERT_EQUAL(Weather::LowVis, Weather::mood(m));
    char l1[24], l2[24];
    Weather::moodLines(Weather::LowVis, m, l1, sizeof(l1), l2, sizeof(l2));
    TEST_ASSERT_EQUAL_STRING("Low visibility", l1);
    TEST_ASSERT_EQUAL_STRING("Fog 300 m", l2);

    // Lifting: 1,200 m and cloud at 400 ft stays low only once it has been.
    TEST_ASSERT_TRUE(Weather::parse("METAR EGLL 240850Z 05004KT 1200 BR BKN004 08/07 Q1025", m));
    TEST_ASSERT_EQUAL(400, m.ceilingFt);
    TEST_ASSERT_EQUAL(Weather::Calm, Weather::mood(m));
    TEST_ASSERT_EQUAL(Weather::LowVis, Weather::mood(m, Weather::LowVis));

    TEST_ASSERT_TRUE(Weather::parse("METAR EGLL 241720Z 24015G35KT 4000 +TSRA BKN014CB 18/16 Q1002", m));
    TEST_ASSERT_TRUE(m.thunder);
    TEST_ASSERT_TRUE(m.cb);
    TEST_ASSERT_EQUAL(1400, m.ceilingFt);
    TEST_ASSERT_EQUAL(Weather::Storm, Weather::mood(m, Weather::LowVis));
    Weather::moodLines(Weather::Storm, m, l1, sizeof(l1), l2, sizeof(l2));
    TEST_ASSERT_EQUAL_STRING("Thunderstorm", l1);
    TEST_ASSERT_EQUAL_STRING("Over Heathrow", l2);

    TEST_ASSERT_TRUE(Weather::parse("METAR EGLL 241750Z 22010KT 9999 -RA VCTS FEW020 SCT030CB 17/15 Q1003", m));
    TEST_ASSERT_FALSE(m.thunder);
    TEST_ASSERT_EQUAL(-1, m.ceilingFt);   // few and scattered: no ceiling
    TEST_ASSERT_EQUAL(Weather::StormNear, Weather::mood(m));
}

void test_qnh_heights()
{
    // 991 hPa: a pressure altitude of 684 ft on the runway is really 83 ft.
    Weather::setQnh(991);
    TEST_ASSERT_FLOAT_WITHIN(2.0, 83.0, Weather::qnhAltitudeM(684 / 3.28084) * 3.28084);
    // A flight level stays a flight level; halfway through the fade, half.
    TEST_ASSERT_FLOAT_WITHIN(1.0, 35000.0, Weather::qnhAltitudeM(35000 / 3.28084) * 3.28084);
    TEST_ASSERT_FLOAT_WITHIN(2.0, 6500.0 - 300.4, Weather::qnhAltitudeM(6500 / 3.28084) * 3.28084);
    // High pressure lifts them; no report, no change.
    Weather::setQnh(1030);
    TEST_ASSERT_FLOAT_WITHIN(2.0, 1453.0, Weather::qnhAltitudeM(1000 / 3.28084) * 3.28084);
    Weather::setQnh(0);
    TEST_ASSERT_FLOAT_WITHIN(0.01, 1000.0, Weather::qnhAltitudeM(1000 / 3.28084) * 3.28084);
}

// -- The airframe collection (9.0) -----------------------------------------
void test_airframes()
{
    struct Mem : Airframes::Store
    {
        std::vector<Airframes::Slot> slots = std::vector<Airframes::Slot>(Airframes::kSlots);
        bool read(uint32_t i, Airframes::Slot &s) override { s = slots[i]; return true; }
        bool write(uint32_t i, const Airframes::Slot &s) override { slots[i] = s; return true; }
    } m;
    TEST_ASSERT_EQUAL(Airframes::New, Airframes::visit(m, 0x406B62, 2490, "G-XWBA", nullptr, "BAW"));
    TEST_ASSERT_EQUAL(Airframes::AgainToday, Airframes::visit(m, 0x406B62, 2490, "G-XWBA", nullptr, "BAW"));
    // The type turns up later (from another feed): filled in, same day.
    TEST_ASSERT_EQUAL(Airframes::Known, Airframes::visit(m, 0x406B62, 2490, "G-XWBA", "A35K", "BAW"));
    TEST_ASSERT_EQUAL(Airframes::Known, Airframes::visit(m, 0x406B62, 2492, "G-XWBA", "A35K", "BAW"));
    int found = 0;
    for (const Airframes::Slot &s : m.slots)
        if (s.key)
        {
            ++found;
            TEST_ASSERT_EQUAL(2490, s.firstDay);
            TEST_ASSERT_EQUAL(2492, s.lastDay);
            TEST_ASSERT_EQUAL(2, s.days);
            TEST_ASSERT_EQUAL_STRING_LEN("G-XWBA", s.reg, 6);
            TEST_ASSERT_EQUAL_STRING_LEN("A35K", s.type, 4);
            TEST_ASSERT_EQUAL_STRING_LEN("BAW", s.op, 3);
        }
    TEST_ASSERT_EQUAL(1, found);
    // A seven-letter registration fills the field with no terminator.
    TEST_ASSERT_EQUAL(Airframes::New, Airframes::visit(m, 0x780A3B, 2492, "B-18901", "A359", "CAL"));
}

void test_taf_and_runway_outlook()
{
    const time_t day = 1792800000;            // 24 Oct 2026 00:00 UTC
    const time_t now = day + 11 * 3600 + 20 * 60;
    // Heathrow's TAF that morning, as aviationweather.gov sends it (a line at a time).
    static const char *const kLines[] = {
        "TAF EGLL 241053Z 2412/2518 20010KT 9999 SCT030", "BECMG 2412/2415 20020G32KT",
        "TEMPO 2412/2423 6000 RA BKN012", "PROB40", "TEMPO 2412/2422 2500 +RA BKN009", "PROB30",
        "TEMPO 2415/2421 21024G42KT", "BECMG 2420/2423 25018G30KT", "BECMG 2504/2507 28022G33KT",
        "PROB30", "TEMPO 2509/2515 27026G43KT", "BECMG 2516/2518 24013KT", "PROB40", "TEMPO 2516/2518 25015G26KT",
    };
    Taf t;
    Weather::tafBegin(t);
    for (const char *l : kLines) Weather::tafFeed(t, l, now);
    TEST_ASSERT_TRUE(t.valid);
    TEST_ASSERT_EQUAL(5, t.n);                 // the base and four BECMGs; no TEMPO or PROB
    TEST_ASSERT_EQUAL((long)(day + 12 * 3600), (long)t.from);
    TEST_ASSERT_EQUAL((long)(day + 86400 + 18 * 3600), (long)t.to);
    TEST_ASSERT_EQUAL((long)(day + 13 * 3600 + 1800), (long)t.w[1].at);   // halfway through 12-15
    TEST_ASSERT_EQUAL(20, t.w[1].kt);
    TEST_ASSERT_EQUAL(32, t.w[1].gust);
    Taf::Wind w;
    TEST_ASSERT_TRUE(Weather::tafWindAt(t, day + 86400 + 6 * 3600, w));
    TEST_ASSERT_EQUAL(280, w.dir);
    Weather::Outlook o;
    TEST_ASSERT_TRUE(Weather::runwayOutlook(t, 270, now, o));
    TEST_ASSERT_FALSE(o.change);               // south-westerlies all day: no change

    // The wind going round to the east this evening.
    Weather::tafBegin(t);
    Weather::tafFeed(t, "TAF EGLL 240500Z 2406/2512 24008KT 9999 FEW030 BECMG 2414/2416 VRB03KT "
                        "FM241900 07012KT 9999 BKN020", now);
    TEST_ASSERT_EQUAL(3, t.n);
    TEST_ASSERT_TRUE(Weather::runwayOutlook(t, 270, now, o));
    TEST_ASSERT_TRUE(o.change);
    TEST_ASSERT_EQUAL((long)(day + 19 * 3600), (long)o.at);
    TEST_ASSERT_EQUAL_STRING("09", o.to);
    TEST_ASSERT_EQUAL(70, o.windDir);
    // On easterlies already, Heathrow goes back west as soon as the tailwind
    // on 27 is under 5 kt: now, with the south-westerly.
    TEST_ASSERT_TRUE(Weather::runwayOutlook(t, 90, now, o));
    TEST_ASSERT_TRUE(o.change);
    TEST_ASSERT_EQUAL((long)now, (long)o.at);
    TEST_ASSERT_EQUAL_STRING("27", o.to);
    // A day later the TAF has run out.
    TEST_ASSERT_FALSE(Weather::runwayOutlook(t, 270, now + 3 * 86400, o));
}

// ── Approach model ─────────────────────────────────────────────────────────
void test_approach_on_final_27L()
{
    const ApproachStatus st = ApproachModel::evaluate(flight(onFinal("a1", "27L", 5)));
    TEST_ASSERT_TRUE(st.phase == ApproachStatus::Approach || st.phase == ApproachStatus::Landing);
    TEST_ASSERT_EQUAL_STRING("27L", st.runway);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 5.0f, st.distKm);
    TEST_ASSERT_TRUE(st.etaSec > 60 && st.etaSec < 200);
}

void test_approach_easterly_09R()
{
    const ApproachStatus st = ApproachModel::evaluate(flight(onFinal("a2", "09R", 8)));
    TEST_ASSERT_EQUAL_STRING("09R", st.runway);
}

void test_overflight_is_not_an_approach()
{
    // High and crossing the field north to south.
    const StateVector s = at("a3", kLat, kLon, 0, 4, 9000, 300, 180, 0);
    const ApproachStatus st = ApproachModel::evaluate(flight(s));
    TEST_ASSERT_FALSE(st.phase == ApproachStatus::Approach || st.phase == ApproachStatus::Landing);
}

void test_climbing_out_27R()
{
    // 3 km past the west end of the north runway, climbing westbound.
    const RunwayEnd *r = nullptr;
    for (int i = 0; i < g_airport.runwayCount; ++i)
        if (!strcmp(g_airport.runways[i].name, "09L")) r = &g_airport.runways[i];   // west end of the north runway
    TEST_ASSERT_NOT_NULL(r);
    const StateVector s = at("d1", r->lat, r->lon, -3, 0, 1500, 170, 270, 2000);
    char rwy[4] = "";
    TEST_ASSERT_TRUE(ApproachModel::climbingOut(flight(s), rwy, sizeof(rwy)));
    TEST_ASSERT_EQUAL_STRING("27R", rwy);
}

// ── Runway tracker ─────────────────────────────────────────────────────────
void test_runway_tracker_follows_and_flips()
{
    RunwayTracker t;
    unsigned long now = 100000;
    // Westerly: arrivals on 27L for ten minutes.
    for (int i = 0; i < 60; ++i, now += 10000)
    {
        std::vector<StateVector> v{onFinal("w1", "27L", 6 - (i % 6)), onFinal("w2", "27L", 10)};
        t.update(v, now);
    }
    TEST_ASSERT_EQUAL_STRING("27L", t.mainArrival());
    // Easterly from now on: two aircraft must agree before it is announced.
    bool announced = false;
    for (int i = 0; i < 60 && !announced; ++i, now += 10000)
    {
        std::vector<StateVector> v{onFinal(i < 30 ? "e1" : "e2", "09L", 7 - (i % 6))};
        announced = t.update(v, now);
    }
    TEST_ASSERT_TRUE(announced);
    TEST_ASSERT_EQUAL_STRING("09L", t.mainArrival());
    TEST_ASSERT_EQUAL_STRING("27L", t.changeFrom());
}

// ── Holding ─────────────────────────────────────────────────────────────────
void test_hold_detected_by_turning()
{
    HoldTracker h;
    unsigned long now = 50000;
    // Circling 15 km north of the airport at 8,000 ft, 15 degrees every 5 s.
    for (int i = 0; i < 40; ++i, now += 5000)
    {
        const double a = i * 15.0 * M_PI / 180.0;
        std::vector<StateVector> v{at("h1", kLat, kLon, 3 * sin(a), 15 + 3 * cos(a), 8000, 220, fmod(i * 15.0 + 90, 360), 0)};
        h.noteStates(v, now);
    }
    TEST_ASSERT_EQUAL(1, h.total(now));
    // A straight flight past the same spot is not holding.
    HoldTracker s;
    for (int i = 0; i < 40; ++i, now += 5000)
    {
        std::vector<StateVector> v{at("s1", kLat, kLon, -10 + i * 0.5, 15, 8000, 220, 90, 0)};
        s.noteStates(v, now);
    }
    TEST_ASSERT_EQUAL(0, s.total(now));
}

// ── readsb / adsb.lol parsing ──────────────────────────────────────────────
void test_parse_stream()
{
    StringStream in(R"({"ac":[
{"hex":"406f79","flight":"BAW242  ","r":"G-ZBKM","t":"B789","alt_baro":2650,"gs":159.0,"track":269.6,"baro_rate":-704,"lat":51.4649,"lon":-0.35,"seen_pos":0.4},
{"hex":"4ca1b2","flight":"EIN15K  ","alt_baro":"ground","lat":51.47,"lon":-0.45,"seen_pos":1.0},
{"hex":"3c66b1","flight":"DLH4LK  ","alt_baro":36000,"lat":53.9,"lon":2.1,"seen_pos":0.2},
{"hex":"40775d","flight":"SHT8   ","alt_baro":3000,"lat":51.5,"lon":-0.5,"seen_pos":45}
],"msg":"No error","now":1792331720,"total":4})");
    std::vector<StateVector> out;
    const int n = ReadsbParser::parseStream(in, "ac", kLat, kLon, 20, out);
    TEST_ASSERT_EQUAL(4, n);                 // all four read
    TEST_ASSERT_EQUAL(1, (int)out.size());   // on the ground, out of range and stale dropped
    TEST_ASSERT_EQUAL_STRING("BAW242", out[0].callsign.c_str());
    TEST_ASSERT_EQUAL_STRING("B789", out[0].aircraft_type.c_str());
    TEST_ASSERT_FLOAT_WITHIN(1.0, 2650 / 3.28084, out[0].baro_altitude);
    // The rest of the reply is still there for the caller to finish.
    TEST_ASSERT_TRUE(in.find("}"));
}

void test_parse_stream_capacity()
{
    std::string many = R"({"ac":[)";
    for (int i = 0; i < 6; ++i)
    {
        char one[160];
        snprintf(one, sizeof(one), R"(%s{"hex":"a%d","lat":51.47,"lon":%.3f,"alt_baro":3000,"seen_pos":1})",
                 i ? "," : "", i, -0.45 + i * 0.01);
        many += one;
    }
    many += "]}";
    std::vector<StateVector> grow;
    StringStream a(many);
    ReadsbParser::parseStream(a, "ac", kLat, kLon, 20, grow);
    TEST_ASSERT_EQUAL(6, (int)grow.size());
    std::vector<StateVector> capped;
    capped.reserve(4);
    StringStream b(many);
    ReadsbParser::parseStream(b, "ac", kLat, kLon, 20, capped);
    TEST_ASSERT_EQUAL(4, (int)capped.size());
}

void test_parse_stream_empty_and_missing()
{
    StringStream empty(R"({"ac":[],"msg":"No error"})");
    std::vector<StateVector> out;
    TEST_ASSERT_EQUAL(0, ReadsbParser::parseStream(empty, "ac", kLat, kLon, 20, out));
    StringStream other(R"({"now":1,"aircraft":[{"hex":"1","lat":51.47,"lon":-0.45,"alt_baro":1000,"seen_pos":1}]})");
    TEST_ASSERT_EQUAL(-1, ReadsbParser::parseStream(other, "ac", kLat, kLon, 20, out));
    StringStream fi(R"({"now":1,"aircraft":[{"hex":"1","lat":51.47,"lon":-0.45,"alt_baro":1000,"seen_pos":1}]})");
    TEST_ASSERT_EQUAL(1, ReadsbParser::parseStream(fi, "aircraft", kLat, kLon, 20, out));
}

// ── Airport packs ──────────────────────────────────────────────────────────
void test_pack_header()
{
    Airport a;
    String err;
    TEST_ASSERT_TRUE(AirportPack::parseHeader(
        R"({"v":1,"icao":"egkk","iata":"lgw","name":"gatwick","lat":51.148,"lon":-0.19,"tz":"GMT0BST,M3.5.0/1,M10.5.0",)"
        R"("runways":[{"n":"26L","lat":51.15,"lon":-0.17,"crs":257.8},{"n":"08R","lat":51.14,"lon":-0.21,"crs":77.8}],)"
        R"("map":{"n":51.25,"s":51.05,"w":-0.51,"e":0.13},"holds":[{"n":"WILLO","lat":50.985,"lon":-0.191}]})", a, err));
    TEST_ASSERT_EQUAL_STRING("EGKK", a.icao);
    TEST_ASSERT_EQUAL_STRING("GATWICK", a.name);
    TEST_ASSERT_EQUAL(2, a.runwayCount);
    TEST_ASSERT_EQUAL(1, a.holdCount);
    TEST_ASSERT_FALSE(AirportPack::parseHeader(R"({"v":1,"name":"nowhere"})", a, err));
    TEST_ASSERT_TRUE(err.length() > 0);
    TEST_ASSERT_FALSE(AirportPack::parseHeader("not json", a, err));
}

// ── Replay: a recorded hour of Heathrow ────────────────────────────────────
// test/fixtures/heathrow.jsonl: adsb.lol around Heathrow every 10 s
// ({"t":unix,"ac":[readsb aircraft]}). The count it should give is worked
// out independently in Python (see test/fixtures/README.md): every aircraft
// that came down below 1,000 ft heading west or east within 6 km.
void test_replay_counts_arrivals()
{
    std::ifstream f("test/fixtures/heathrow.jsonl");
    if (!f) TEST_IGNORE_MESSAGE("no fixture (test/fixtures/heathrow.jsonl)");
    std::ifstream e("test/fixtures/heathrow.expected");
    int expected = 0;
    e >> expected;

    static DailyStats stats;
    RunwayTracker runways;
    std::string line;
    unsigned long now = 0;
    std::vector<std::string> onFinal;   // aircraft the approach model put on final within 12 km
    while (std::getline(f, line))
    {
        StringStream in(line);
        std::vector<StateVector> v;
        const int got = ReadsbParser::parseStream(in, "ac", kLat, kLon, 20, v);
        if (got < 0) continue;
        int inLine = 0;
        for (size_t p = line.find("\"hex\""); p != std::string::npos; p = line.find("\"hex\"", p + 1)) ++inLine;
        static int shown = 0;
        if (got != inLine && shown++ < 5) printf("line %lu: parsed %d of %d aircraft\n", now / 10000, got, inLine);
        now += 10000;
        g_fakeMillis = now;
        stats.noteTraffic(v);
        runways.update(v, now);
        for (const StateVector &s : v)
        {
            const ApproachStatus st = ApproachModel::evaluate(flight(s));
            if ((st.phase == ApproachStatus::Approach || st.phase == ApproachStatus::Landing) && st.distKm < 12 &&
                s.baro_altitude * 3.28084 <= 5000 &&
                std::find(onFinal.begin(), onFinal.end(), s.icao24.c_str()) == onFinal.end())
                onFinal.push_back(s.icao24.c_str());
        }
    }
    printf("on final within 12 km (%d):", (int)onFinal.size());
    for (auto &h : onFinal) printf(" %s", h.c_str());
    printf("\n");
    printf("replay: %d arrivals, %d departures (independent count %d), landing on %s\n",
           stats.arrivals(), stats.departures(), expected, runways.mainArrival());
    printf("replay: %d go-arounds\n", stats.goArounds());
    TEST_ASSERT_EQUAL(0, stats.goArounds());   // real traffic, none went around: no false alarms
    TEST_ASSERT_TRUE(expected > 0);
    // Within 15% of the independent count.
    TEST_ASSERT_INT_WITHIN(expected * 15 / 100 + 1, expected, stats.arrivals());
    TEST_ASSERT_TRUE(runways.mainArrival()[0] != '\0');
    // Every counted arrival had a runway: the runway shares add up.
    DailyStats::RunwayCount rw[4];
    const int nr = stats.runways(rw, 4);
    int sum = 0;
    for (int i = 0; i < nr; ++i) sum += rw[i].n;
    TEST_ASSERT_TRUE(nr >= 1);
    TEST_ASSERT_EQUAL(stats.arrivals(), sum);
    TEST_ASSERT_TRUE(nr < 2 || rw[0].n >= rw[1].n);
}

void test_logbook()
{
    LittleFS.wipe();
    Logbook::begin();
    char l1[20], l2[24];
    auto tally = [](const char *code, uint8_t kind, uint16_t n) {
        Logbook::Tally t{};
        strlcpy(t.code, code, sizeof(t.code));
        t.kind = kind; t.n = n;
        return t;
    };
    // The first day: kept, no news (nothing to beat yet).
    Logbook::Day d1;
    d1.date = 20261020; d1.arrivals = 600; d1.departures = 590; d1.goArounds = 1; d1.tightestSec = 70;
    std::vector<Logbook::Tally> t1 = {tally("BA", 0, 300), tally("A320", 1, 200)};
    Logbook::addDay(d1, t1, l1, sizeof(l1), l2, sizeof(l2));
    TEST_ASSERT_EQUAL_STRING("", l1);
    TEST_ASSERT_EQUAL(1190, Logbook::records().bestDay);
    TEST_ASSERT_EQUAL(1, Logbook::records().airlines);
    // A busier day: a new record, BA seen again, Virgin new.
    Logbook::Day d2 = d1;
    d2.date = 20261021; d2.arrivals = 640; d2.departures = 610; d2.tightestSec = 64;
    std::vector<Logbook::Tally> t2 = {tally("BA", 0, 310), tally("VS", 0, 20), tally("A320", 1, 210)};
    Logbook::addDay(d2, t2, l1, sizeof(l1), l2, sizeof(l2));
    TEST_ASSERT_EQUAL_STRING("New record", l1);
    TEST_ASSERT_EQUAL_STRING("1250 movements", l2);
    TEST_ASSERT_EQUAL(64, Logbook::records().tightest);
    TEST_ASSERT_EQUAL(2, Logbook::records().airlines);
    TEST_ASSERT_EQUAL(1, Logbook::records().types);
    // The same day handed over again (a restart after midnight): not counted twice.
    Logbook::addDay(d2, t2, l1, sizeof(l1), l2, sizeof(l2));
    Logbook::Event e;
    e.at = 1792350900; e.kind = DailyStats::GoAround;
    strlcpy(e.ident, "BA117", sizeof(e.ident)); strlcpy(e.what, "27L", sizeof(e.what));
    Logbook::addEvent(e);
    // Read back as the web page does.
    WiFiClient days, events, coll;
    Logbook::writeDaysCsv(days);
    Logbook::writeEventsCsv(events);
    Logbook::writeCollectionCsv(coll);
    TEST_ASSERT_TRUE(days.sent.find("\n2026-10-20,600,590,1,") != std::string::npos);
    TEST_ASSERT_TRUE(days.sent.find("\n2026-10-21,640,610,") != std::string::npos);
    const std::string body = days.sent.substr(days.sent.find("\r\n\r\n") + 4);   // after the HTTP header
    TEST_ASSERT_EQUAL(3, (int)std::count(body.begin(), body.end(), '\n'));       // the CSV header and two days
    TEST_ASSERT_TRUE(events.sent.find(",go-around,BA117,27L") != std::string::npos);
    TEST_ASSERT_TRUE(coll.sent.find("airline,BA,2026-10-20,2026-10-21,2,610") != std::string::npos);
    TEST_ASSERT_TRUE(coll.sent.find("airline,VS,2026-10-21,2026-10-21,1,20") != std::string::npos);
    LittleFS.wipe();
}

void test_go_around_from_traffic()
{
    // On final for 27L, then a little later climbing away past the
    // threshold: counted, logged, reported once, whether or not it was
    // the card's flight (8.1).
    static DailyStats s;
    std::vector<StateVector> v(1);
    v[0] = onFinal("ga1", "27L", 3);
    s.noteTraffic(v);
    v[0] = onFinal("ga1", "27L", 1.5);
    s.noteTraffic(v);
    TEST_ASSERT_EQUAL(0, s.goArounds());
    v[0] = onFinal("ga1", "27L", 0.2);
    v[0].baro_altitude = 900 / 3.28084;          // climbing away
    v[0].vertical_rate = 2000 / 196.85;
    s.noteTraffic(v);
    TEST_ASSERT_EQUAL(1, s.goArounds());
    char id[10], rwy[4];
    int alt = 0;
    TEST_ASSERT_TRUE(s.takeGoAround(id, sizeof(id), rwy, sizeof(rwy), alt));
    TEST_ASSERT_EQUAL_STRING("TSTga1", id);
    TEST_ASSERT_EQUAL_STRING("27L", rwy);
    TEST_ASSERT_FALSE(s.takeGoAround(id, sizeof(id), rwy, sizeof(rwy), alt));   // reported once
    v[0].baro_altitude = 1400 / 3.28084;         // still climbing: not a second one
    s.noteTraffic(v);
    TEST_ASSERT_EQUAL(1, s.goArounds());
    TEST_ASSERT_EQUAL(DailyStats::GoAround, s.spot(0).kind);
}

void test_spotter_log()
{
    static DailyStats s;
    s.noteSpot(DailyStats::Rare, "QR7", "A388");
    s.noteSpot(DailyStats::Rare, "QR7", "A388");          // the same sighting again: once
    s.noteSpot(DailyStats::FirstVisit, "QR7", "A7-APA");  // another kind: kept
    s.noteSpot(DailyStats::GoAround, "BA117", "27L");
    s.noteSpot(DailyStats::GoAround, "BA117", "27L");     // go-arounds always count
    TEST_ASSERT_EQUAL(4, s.spotCount());
    TEST_ASSERT_EQUAL_STRING("BA117", s.spot(0).ident);
    TEST_ASSERT_EQUAL_STRING("A388", s.spot(3).what);
    char id[8];
    for (int i = 0; i < 30; ++i) { snprintf(id, sizeof(id), "X%d", i); s.noteSpot(DailyStats::Overhead, id, "A320"); }
    TEST_ASSERT_EQUAL(DailyStats::kMaxSpots, s.spotCount());   // the oldest dropped off
    TEST_ASSERT_EQUAL_STRING("X29", s.spot(0).ident);
    TEST_ASSERT_EQUAL(34, s.spotsToday());
    // A busy run of overhead passes never pushes out the rare things (8.1).
    int goArounds = 0, rare = 0;
    for (int i = 0; i < s.spotCount(); ++i)
    {
        goArounds += s.spot(i).kind == DailyStats::GoAround;
        rare += s.spot(i).kind == DailyStats::Rare;
    }
    TEST_ASSERT_EQUAL(2, goArounds);
    TEST_ASSERT_EQUAL(1, rare);
}

// ── Robustness: random and mangled input must never crash a parser ──────────
void test_fuzz_parsers()
{
    srand(12345);
    const std::string good = R"({"ac":[{"hex":"406f79","flight":"BAW242  ","alt_baro":2650,"gs":159.0,"track":269.6,)"
                             R"("baro_rate":-704,"lat":51.4649,"lon":-0.35,"seen_pos":0.4}],"msg":"No error"})";
    const char *metar = "METAR EGLL 151720Z AUTO 21012G25KT 180V250 9999 -RA SCT043 17/15 Q1019";
    const char *pack = R"({"v":1,"icao":"EGKK","name":"GATWICK","lat":51.148,"lon":-0.19,"runways":[{"n":"26L","lat":51.15,"lon":-0.17,"crs":257.8}],"map":{"n":51.25,"s":51.05,"w":-0.51,"e":0.13}})";
    for (int round = 0; round < 3000; ++round)
    {
        // Mangle a copy: flip, drop, insert or truncate bytes.
        auto mangle = [](std::string t) {
            const int edits = 1 + rand() % 8;
            for (int k = 0; k < edits && !t.empty(); ++k)
            {
                const size_t at = rand() % t.size();
                switch (rand() % 4)
                {
                    case 0: t[at] = (char)(rand() % 256); break;
                    case 1: t.erase(at, 1 + rand() % 4); break;
                    case 2: t.insert(at, 1, "{}[],:\"0-9e"[rand() % 12]); break;
                    case 3: t.resize(at); break;
                }
            }
            return t;
        };
        std::string junk(rand() % 200, ' ');
        for (char &c : junk) c = (char)(rand() % 256);

        std::vector<StateVector> out;
        StringStream a(mangle(good)), b(junk);
        ReadsbParser::parseStream(a, "ac", kLat, kLon, 20, out);
        ReadsbParser::parseStream(b, "ac", kLat, kLon, 20, out);
        Metar m;
        Weather::parse(mangle(metar).c_str(), m);
        Weather::parse(junk.c_str(), m);
        Airport ap{};   // parseHeader leaves it untouched when the JSON is broken
        String err;
        AirportPack::parseHeader(mangle(pack).c_str(), ap, err);
        TEST_ASSERT_TRUE(ap.runwayCount <= Airport::kMaxRunways && ap.holdCount <= Airport::kMaxHolds);
    }
}

// The offline airport table: lookups both ways, cities, short forms.
void test_airport_time_zones()
{
    struct Case { const char *code; time_t t; int minutes; };
    const time_t oct = 1792350900;   // 18 Oct 2026, 19:15 UTC
    const time_t jan = 1798761600;   // 1 Jan 2027, 00:00 UTC
    const Case cases[] = {
        {"LHR", oct, 60},   {"LHR", jan, 0},     // BST until 25 Oct
        {"DXB", oct, 240},  {"DXB", jan, 240},   // no daylight saving
        {"JFK", oct, -240}, {"JFK", jan, -300},  // EDT until 1 Nov
        {"SYD", oct, 660},  {"SYD", jan, 660},   // AEDT from 4 Oct
        {"AKL", oct, 780},  {"AKL", jan, 780},   // NZDT from 27 Sep
        {"GRU", oct, -180}, {"DEL", oct, 330},   // Brazil no longer changes; India +5:30
    };
    for (const Case &k : cases)
    {
        Airports::Place p;
        TEST_ASSERT_TRUE_MESSAGE(Airports::byIata(k.code, p), k.code);
        TEST_ASSERT_EQUAL_INT_MESSAGE(k.minutes, Airports::utcOffsetMinutes(p, k.t), k.code);
    }
}

void test_airport_cities()
{
    Airports::Place p;
    TEST_ASSERT_TRUE(Airports::byIata("LHR", p));
    TEST_ASSERT_EQUAL_STRING("EGLL", p.icao);
    TEST_ASSERT_EQUAL_STRING("London", p.city);
    TEST_ASSERT_FLOAT_WITHIN(0.02f, 51.47f, p.lat);
    TEST_ASSERT_TRUE(Airports::byIcao("kjfk", p));
    TEST_ASSERT_EQUAL_STRING("JFK", p.iata);
    TEST_ASSERT_EQUAL_STRING("New York", p.city);
    TEST_ASSERT_EQUAL_STRING("Brussels", Airports::city("BRU"));   // municipality is Zaventem
    TEST_ASSERT_EQUAL_STRING("Edinburgh", Airports::city("EGPH"));
    TEST_ASSERT_NULL(Airports::city("QQQ"));
    TEST_ASSERT_NULL(Airports::city("LH"));
    char iata[4];
    TEST_ASSERT_TRUE(Airports::icaoToIata("LFPG", iata));
    TEST_ASSERT_EQUAL_STRING("CDG", iata);
    TEST_ASSERT_FALSE(Airports::icaoToIata("ZZZZ", iata));
    TEST_ASSERT_EQUAL_STRING("", iata);

    char s[8];
    TEST_ASSERT_TRUE(Airports::shortCity("DUB", 7, s, sizeof(s)));
    TEST_ASSERT_EQUAL_STRING("Dublin", s);
    TEST_ASSERT_TRUE(Airports::shortCity("JFK", 7, s, sizeof(s)));
    TEST_ASSERT_EQUAL_STRING("NewYork", s);
    TEST_ASSERT_TRUE(Airports::shortCity("SFO", 7, s, sizeof(s)));
    TEST_ASSERT_EQUAL_STRING("San Fra", s);
    TEST_ASSERT_TRUE(Airports::shortCity("LAX", 7, s, sizeof(s)));
    TEST_ASSERT_EQUAL_STRING("Los Ang", s);
    TEST_ASSERT_FALSE(Airports::shortCity("QQQ", 7, s, sizeof(s)));

    // Cities found through the IATA index are real, short strings.
    for (const char *code : { "AMS", "SIN", "SYD", "GRU", "NBO", "YVR" })
    {
        const char *c = Airports::city(code);
        TEST_ASSERT_NOT_NULL(c);
        TEST_ASSERT_TRUE(strlen(c) > 1 && strlen(c) <= 15);
    }
}

// ── Spotter ────────────────────────────────────────────────────────────────
void test_watchlist_matching()
{
    StateVector s;
    s.icao24 = "43c6f1";
    s.callsign = "RRR6754";
    s.registration = "ZZ336";
    s.aircraft_type = "A332";
    char m[16];
    TEST_ASSERT_TRUE(Spotter::watchMatch("g-civa, rrr*", s, m, sizeof(m)));
    TEST_ASSERT_EQUAL_STRING("rrr*", m);
    TEST_ASSERT_TRUE(Spotter::watchMatch("Z-Z336", s, m, sizeof(m)));      // dashes ignored
    TEST_ASSERT_TRUE(Spotter::watchMatch("A33*", s, m, sizeof(m)));
    TEST_ASSERT_TRUE(Spotter::watchMatch("43C6F1", s, m, sizeof(m)));
    TEST_ASSERT_FALSE(Spotter::watchMatch("A333 B74*", s, m, sizeof(m)));
    TEST_ASSERT_FALSE(Spotter::watchMatch("RRR", s, m, sizeof(m)));       // a call sign needs * to be a prefix
    TEST_ASSERT_FALSE(Spotter::watchMatch("*", s, m, sizeof(m)));
    TEST_ASSERT_FALSE(Spotter::watchMatch("", s, m, sizeof(m)));
}

void test_overhead_and_cooldown()
{
    Spotter::reset();
    Spotter::Settings cfg;
    cfg.home_lat = 51.50; cfg.home_lon = -0.10;   // central London, east of the airport
    cfg.overhead_km = 2;
    // An overflight 1 km north of home at 30,000 ft, and one 5 km away.
    std::vector<StateVector> v{at("aaa001", 51.50, -0.10, 0, 5, 30000, 450, 90, 0),
                               at("aaa002", 51.50, -0.10, 0, 1, 30000, 450, 90, 0)};
    Spotter::Event e;
    TEST_ASSERT_TRUE(Spotter::check(v, cfg, 1000, e));
    TEST_ASSERT_EQUAL(1, e.index);
    TEST_ASSERT_FALSE(e.watch);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 1.0f, e.distKm);
    // Announced once: the next fetch is quiet, 46 minutes later it can come round again.
    TEST_ASSERT_FALSE(Spotter::check(v, cfg, 6000, e));
    TEST_ASSERT_TRUE(Spotter::check(v, cfg, 1000 + 46UL * 60 * 1000, e));
    // A watchlist match wins over an overhead.
    Spotter::reset();
    v[0].aircraft_type = "B744";
    cfg.watchlist = "B74*";
    TEST_ASSERT_TRUE(Spotter::check(v, cfg, 1000, e));
    TEST_ASSERT_TRUE(e.watch);
    TEST_ASSERT_EQUAL(0, e.index);
    TEST_ASSERT_EQUAL_STRING("B74*", e.matched);
    // Off without a home marker or radius.
    Spotter::reset();
    cfg.watchlist = "";
    cfg.overhead_km = 0;
    TEST_ASSERT_FALSE(Spotter::check(v, cfg, 1000, e));
}

void test_overhead_skips_arrivals()
{
    Spotter::reset();
    // Home under the 27L approach, 6 km out: an arrival passing over is not an overflight.
    const StateVector arr = onFinal("bbb001", "27L", 6);
    Spotter::Settings cfg;
    cfg.home_lat = arr.lat; cfg.home_lon = arr.lon;
    cfg.overhead_km = 2;
    std::vector<StateVector> v{arr};
    Spotter::Event e;
    TEST_ASSERT_FALSE(Spotter::check(v, cfg, 1000, e));
    cfg.overhead_all = true;
    TEST_ASSERT_TRUE(Spotter::check(v, cfg, 1000, e));
}

// ── Registration history ───────────────────────────────────────────────────
struct MemStore : SeenLog::Store
{
    SeenLog::Slot slots[SeenLog::kSlots];
    int writes = 0;
    bool read(uint32_t i, SeenLog::Slot &s) override { s = slots[i]; return true; }
    bool write(uint32_t i, const SeenLog::Slot &s) override { slots[i] = s; ++writes; return true; }
};

void test_seen_log()
{
    static MemStore m;
    SeenLog::Slot before;
    TEST_ASSERT_TRUE(SeenLog::visit(m, 0x4CA9BA, 2480, before));
    TEST_ASSERT_EQUAL(0, before.days);                     // first visit
    TEST_ASSERT_TRUE(SeenLog::visit(m, 0x4CA9BA, 2480, before));
    TEST_ASSERT_EQUAL(1, before.days);                     // same day: counted once
    TEST_ASSERT_EQUAL(1, m.writes);
    TEST_ASSERT_TRUE(SeenLog::visit(m, 0x4CA9BA, 2483, before));
    TEST_ASSERT_EQUAL(1, before.days);
    TEST_ASSERT_EQUAL(2480, before.lastDay);
    TEST_ASSERT_TRUE(SeenLog::visit(m, 0x4CA9BA, 2484, before));
    TEST_ASSERT_EQUAL(2, before.days);
    // Thousands of aircraft: every visit still succeeds (oldest slots reused).
    for (uint32_t a = 1; a <= 5000; ++a)
        TEST_ASSERT_TRUE(SeenLog::visit(m, a * 7919, (uint16_t)(2500 + a / 100), before));
    // 2 Jan 2020 is day 1.
    struct tm t = {};
    t.tm_year = 120; t.tm_mon = 0; t.tm_mday = 2; t.tm_hour = 12;
    TEST_ASSERT_EQUAL(1, SeenLog::dayNumber(mktime(&t)));
}

// ── The web server's request parsing ───────────────────────────────────────
void test_http_request()
{
    const char *CRLF = "\r\n";
    String raw = String("POST /api/config?x=1&z=2 HTTP/1.1") + CRLF + "Host: glideslope.local" + CRLF +
                 "content-length:12" + CRLF + "X-Glideslope:  1 " + CRLF + CRLF;
    HttpRequest::Line l;
    TEST_ASSERT_TRUE(HttpRequest::parseLine(raw, l));
    TEST_ASSERT_EQUAL_STRING("POST", l.method.c_str());
    TEST_ASSERT_EQUAL_STRING("/api/config", l.path.c_str());
    TEST_ASSERT_EQUAL_STRING("x=1&z=2", l.query.c_str());
    TEST_ASSERT_EQUAL_STRING("glideslope.local", HttpRequest::header(raw, "host").c_str());
    TEST_ASSERT_EQUAL_STRING("1", HttpRequest::header(raw, "X-Glideslope").c_str());
    TEST_ASSERT_EQUAL_STRING("", HttpRequest::header(raw, "X-Glide").c_str());   // a prefix is not the header
    TEST_ASSERT_EQUAL(12, HttpRequest::contentLength(raw));                       // no space after the colon
    TEST_ASSERT_EQUAL_STRING("2", HttpRequest::qparam(l.query, "z").c_str());
    TEST_ASSERT_EQUAL_STRING("", HttpRequest::qparam("fz=1", "z").c_str());     // whole keys only
    TEST_ASSERT_EQUAL_STRING("night+storm", HttpRequest::qparam("look=night+storm", "look").c_str());

    // Malformed request lines.
    for (const char *bad : {"", "GET", "GET /", "GET  HTTP/1.1", "GET x HTTP/1.1", " / HTTP/1.1", "GET / "})
        TEST_ASSERT_FALSE(HttpRequest::parseLine(String(bad) + CRLF + CRLF, l));
    // Content-Length that isn't a plain, sane number.
    for (const char *v : {"-5", "abc", "12abc", "", "9999999999", "200000000"})
        TEST_ASSERT_EQUAL(-1, HttpRequest::contentLength(String("GET / HTTP/1.1") + CRLF + "Content-Length: " + v + CRLF + CRLF));
    TEST_ASSERT_EQUAL(-1, HttpRequest::contentLength(String("GET / HTTP/1.1") + CRLF + CRLF));

    // Random bytes and random fragments of requests: nothing may crash, and
    // the results stay inside the input.
    uint32_t seed = 12345;
    auto rnd = [&]() { seed = seed * 1103515245u + 12345u; return (seed >> 16) & 0x7fff; };
    const char *bits[] = {"GET", "POST", " ", "/", "?", "&", "=", ":", "\r\n", "Host", "Content-Length", "X-Glideslope", "HTTP/1.1", "9", "-"};
    for (int iter = 0; iter < 3000; ++iter)
    {
        String r;
        const int parts = rnd() % 40;
        for (int k = 0; k < parts; ++k)
        {
            if (rnd() % 3 == 0) { char ch = (char)(rnd() % 256); if (!ch) ch = 'x'; r += ch; }
            else r += bits[rnd() % (sizeof(bits) / sizeof(bits[0]))];
        }
        HttpRequest::Line out;
        if (HttpRequest::parseLine(r, out))
            TEST_ASSERT_TRUE(out.path.length() <= r.length() && out.path[0] == '/');
        TEST_ASSERT_TRUE(HttpRequest::header(r, "Host").length() <= r.length());
        const long cl = HttpRequest::contentLength(r);
        TEST_ASSERT_TRUE(cl >= -1 && cl <= 100L * 1024 * 1024);
        TEST_ASSERT_TRUE(HttpRequest::qparam(r, "z").length() <= r.length());
    }
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_metar_uk);
    RUN_TEST(test_metar_us);
    RUN_TEST(test_metar_calm_and_garbage);
    RUN_TEST(test_approach_on_final_27L);
    RUN_TEST(test_approach_easterly_09R);
    RUN_TEST(test_overflight_is_not_an_approach);
    RUN_TEST(test_climbing_out_27R);
    RUN_TEST(test_runway_tracker_follows_and_flips);
    RUN_TEST(test_hold_detected_by_turning);
    RUN_TEST(test_parse_stream);
    RUN_TEST(test_parse_stream_capacity);
    RUN_TEST(test_parse_stream_empty_and_missing);
    RUN_TEST(test_pack_header);
    RUN_TEST(test_fuzz_parsers);
    RUN_TEST(test_replay_counts_arrivals);
    RUN_TEST(test_airport_cities);
    RUN_TEST(test_watchlist_matching);
    RUN_TEST(test_overhead_and_cooldown);
    RUN_TEST(test_overhead_skips_arrivals);
    RUN_TEST(test_seen_log);
    RUN_TEST(test_spotter_log);
    RUN_TEST(test_go_around_from_traffic);
    RUN_TEST(test_logbook);
    RUN_TEST(test_airport_time_zones);
    RUN_TEST(test_http_request);
    RUN_TEST(test_metar_mood);
    RUN_TEST(test_taf_and_runway_outlook);
    RUN_TEST(test_qnh_heights);
    RUN_TEST(test_airframes);
    return UNITY_END();
}
