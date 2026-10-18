// Unit tests for the board's logic, built for the desktop with the stand-in
// Arduino layer in test/stubs:   pio test -e native
//
// The sources are compiled into this one test program (they only need a few
// Arduino types, which the stubs provide).
#include <unity.h>
#include <vector>
#include <fstream>

#include "config/RuntimeConfig.h"
RuntimeConfig g_config{};
unsigned long g_fakeMillis = 1000;

#include "../../config/Airport.cpp"
#include "../../display/ApproachModel.cpp"
#include "../../display/RunwayTracker.cpp"
#include "../../display/HoldTracker.cpp"
#include "../../display/DailyStats.cpp"
#include "../../utils/Weather.cpp"
#include "../../adapters/ReadsbParser.cpp"

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
    TEST_ASSERT_EQUAL_STRING("-RA", m.wx);
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
    Metar bad;
    TEST_ASSERT_FALSE(Weather::parse("", bad));
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

    DailyStats stats;
    RunwayTracker runways;
    std::string line;
    unsigned long now = 0;
    while (std::getline(f, line))
    {
        StringStream in(line);
        std::vector<StateVector> v;
        if (ReadsbParser::parseStream(in, "ac", kLat, kLon, 20, v) < 0) continue;
        now += 10000;
        g_fakeMillis = now;
        stats.noteTraffic(v);
        runways.update(v, now);
    }
    printf("replay: %d arrivals, %d departures (independent count %d), landing on %s\n",
           stats.arrivals(), stats.departures(), expected, runways.mainArrival());
    TEST_ASSERT_TRUE(expected > 0);
    // Within 15% of the independent count.
    TEST_ASSERT_INT_WITHIN(expected * 15 / 100 + 1, expected, stats.arrivals());
    TEST_ASSERT_TRUE(runways.mainArrival()[0] != '\0');
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
        Airport ap;
        String err;
        AirportPack::parseHeader(mangle(pack).c_str(), ap, err);
        TEST_ASSERT_TRUE(ap.runwayCount <= Airport::kMaxRunways && ap.holdCount <= Airport::kMaxHolds);
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
    RUN_TEST(test_parse_stream_empty_and_missing);
    RUN_TEST(test_pack_header);
    RUN_TEST(test_fuzz_parsers);
    RUN_TEST(test_replay_counts_arrivals);
    return UNITY_END();
}
