/*
Purpose: Firmware entry point for ESP32.
Responsibilities:
- Initialize serial, connect to Wi‑Fi, and construct fetchers and display.
- Periodically fetch state vectors (OpenSky), enrich flights (AeroAPI), and render.
Configuration: UserConfiguration (location/filters/colors), TimingConfiguration (intervals),
               WiFiConfiguration (SSID/password), HardwareConfiguration (display specs).
*/
#include "utils/Notify.h"
#include <vector>
#include <map>
#include <esp_task_wdt.h>
#include <esp_heap_caps.h>
#include "utils/StageTrace.h"
#include "utils/ImageCheck.h"
#include <WiFi.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "config/WiFiConfiguration.h"
#include "adapters/OpenSkyFetcher.h"
#include "adapters/Tar1090Fetcher.h"
#include "adapters/FallbackStateVectorFetcher.h"
#include "adapters/AdsbAggregatorFetcher.h"
#include "adapters/AeroAPIFetcher.h"
#include "adapters/HexDbFetcher.h"
#include "adapters/OpenSkyRouteFetcher.h"
#include "adapters/FallbackFlightFetcher.h"
#include "adapters/AdsbImRouteFetcher.h"
#include "adapters/RouteBookFetcher.h"
#include "adapters/LocalLogoStore.h"
#include "core/FlightDataFetcher.h"
#include "adapters/NeoMatrixDisplay.h"
#include "display/CardRenderer.h"
#include "display/AircraftSprites.h"
#include "display/ApproachModel.h"
#include "display/RareSpotter.h"
#include "utils/TelnetLogger.h"
#include "utils/WebConfig.h"
#include "utils/WifiProvisioner.h"
#include "utils/Weather.h"
#include "utils/GeoUtils.h"
#include "utils/HeapWatch.h"
#include "config/RuntimeConfig.h"
#include "config/Airport.h"
#include "config/Airports.h"
#include "utils/Logbook.h"
#include "utils/Spotter.h"
#include "utils/HomeAssistant.h"
#include "utils/Ota.h"
#include "utils/Health.h"
#include "utils/Airframes.h"
#include "utils/Liveries.h"
#include "config/Countries.h"
#include "config/AirlineNames.h"
#include "utils/TlsArena.h"
#include "utils/SeenLog.h"
#include "display/OverheadScene.h"
#include "display/FlyAcross.h"

static OpenSkyFetcher             g_openSky;
static Tar1090Fetcher             g_tar1090;
static AdsbAggregatorFetcher      g_feeds;
// Positions: local receiver -> adsb.lol / adsb.fi -> OpenSky.
static FallbackStateVectorFetcher g_feedChain(&g_feeds, &g_openSky);
static FallbackStateVectorFetcher g_stateFetcher(&g_tar1090, &g_feedChain);
static HexDbFetcher               g_hexDb;
static AeroAPIFetcher             g_aeroApi;
static OpenSkyRouteFetcher        g_openSkyRoute(g_openSky);
// Chain: hexdb → AeroAPI (if key set) → OpenSky flights endpoint (if creds set)
static FallbackFlightFetcher      g_aeroApiFallback(&g_aeroApi, &g_openSkyRoute);
static FallbackFlightFetcher      g_flightFetcher(&g_hexDb, &g_aeroApiFallback);
// adsb.im first (plain HTTP, no TLS), then the routes the board has learned
// (flash, 9.0); the HTTPS chain above only for routes neither has.
static AdsbImRouteFetcher         g_adsbIm;
static RouteBookFetcher           g_learnedOrOnline(&g_flightFetcher);
static FallbackFlightFetcher      g_routes(&g_adsbIm, &g_learnedOrOnline);
static LocalLogoStore             g_logoStore;
static FlightDataFetcher         *g_fetcher = nullptr;
static NeoMatrixDisplay g_display;
static TaskHandle_t     g_webTask = nullptr;

// The main loop fetches, parses JSON and, now and then, runs a TLS handshake
// several calls deep: 12 KB rather than Arduino's 8 KB (a stack overflow
// crashed 4.0.0 once in six days).
SET_LOOP_TASK_STACK_SIZE(12 * 1024);

static unsigned long g_lastFetchMs = 0;
static unsigned long g_lastWebSyncMs = 0;
static std::vector<StateVector> g_states;
static std::vector<FlightInfo> g_flights;
static bool   g_wasNightSuppressed = false;  // tracks night-mode suppression for wake-up flush
static String g_wifiSsid;                   // active SSID  (NVS > compile-time)
static String g_wifiPass;                   // active password
static Metar  g_metar;                      // Heathrow weather, refreshed every few minutes
static Taf    g_taf;                        // and its forecast (8.2: the runway outlook)
static uint8_t g_mood = Weather::Calm;      // the airfield's mood from the METAR (8.2)
static unsigned long g_lastMetarMs = 0;
static constexpr unsigned long kMetarEveryMs = 10UL * 60 * 1000;
// 9.0: timed to the next report (Heathrow issues one every 30 minutes):
// each fetch closes and reopens the feed's secure connection, and those
// reopens are where the heap goes to its lowest.
static unsigned long g_metarWaitMs = kMetarEveryMs;
static char g_originIds[24] = "";   // where the next arrivals came from, for the weather fetch
static char g_cardOrigin[5] = "";   // the card's origin (ICAO), to have its weather in time


// Emergency squawks: 7500 unlawful interference, 7600 radio failure, 7700
// general emergency. Returns the panel wording, or nullptr for any other code.
static const char *squawkMeaning(const String &sq)
{
    if (sq == "7500") return "HIJACK";
    if (sq == "7600") return "RADIO FAIL";
    if (sq == "7700") return "EMERGENCY";
    return nullptr;
}

// Raise the panel alert for an emergency squawk once it has been seen in two
// fetches running (a single garbled reply is ignored), then not again for
// that aircraft and code for 30 minutes.
// Holding stacks (Airport::holds): whose turn, and the sample itself.
static unsigned long g_lastStackMs = 0;
static int           g_nextStack = 0;

static bool stackDue(unsigned long now)
{
    return g_airport.holdCount && g_config.use_community_feeds && !g_feeds.holdOffMs() &&
           (!g_lastStackMs || now - g_lastStackMs >= 45000UL) && ESP.getFreeHeap() > 26000;
}

static void sampleStack()
{
    StageTrace::mark(StageTrace::Main, StageTrace::MainStack);
    g_lastStackMs = millis();
    const HoldFix &fix = g_airport.holds[g_nextStack];
    std::vector<StateVector> around;
    bool ok;
    ok = g_feeds.fetchArea(fix.lat, fix.lon, 9.0, around);   // (pauses the web server itself if it must connect)
    if (ok) g_display.noteStack(g_nextStack, around);
    g_nextStack = (g_nextStack + 1) % g_airport.holdCount;
    heapCheckpoint("stack sample");
}

// What Home Assistant sees (utils/HomeAssistant), refreshed once per fetch.
static HomeAssistant::State g_ha;

// Registration history for the flights about to get cards: days each
// airframe has been on the board. One small file read per new aircraft a
// day; remembered here so later fetches don't touch the file.
static void noteHistory(const std::vector<StateVector> &states, std::vector<FlightInfo> &flights)
{
    const time_t now = time(nullptr);
    if (now < 1600000000 || flights.empty()) return;
    const uint16_t today = SeenLog::dayNumber(now);
    // The day the history began (kept in NVS): until it is a week old,
    // nearly everything is new to it, so "FIRST VISIT" would say little.
    static uint16_t historyStart = 0;
    if (!historyStart)
    {
        Preferences p;
        p.begin("seenlog", false);
        historyStart = (uint16_t)p.getUInt("start", 0);
        if (!historyStart) { historyStart = today; p.putUInt("start", today); }
        p.end();
    }
    const bool youngHistory = today - historyStart < 7;
    struct Known { uint32_t icao; uint16_t day, days, prev; };
    static Known known[24] = {};
    static uint8_t next = 0;
    for (FlightInfo &f : flights)
    {
        uint32_t icao = 0;
        for (const StateVector &s : states)
            if (s.callsign == f.ident) { icao = (uint32_t)strtoul(s.icao24.c_str(), nullptr, 16); break; }
        if (!icao) continue;
        const Known *k = nullptr;
        for (const Known &c : known)
            if (c.icao == icao && c.day == today) { k = &c; break; }
        if (!k)
        {
            SeenLog::Slot before;
            if (!SeenLog::visit(SeenLog::fileStore(), icao, today, before)) continue;
            Known &c = known[next];
            next = (next + 1) % 24;
            c.icao = icao;
            c.day = today;
            c.days = before.lastDay == today ? before.days : before.days + 1;
            c.prev = before.lastDay == today ? 0 : before.lastDay;
            k = &c;
        }
        f.seen_days = (k->days == 1 && youngHistory) ? 0 : k->days;   // 0: nothing shown
        f.seen_prev_day = k->prev;
    }
}

// Overhead and watchlist: the pass on the panel, and a phone alert for the watchlist.
// The collection (9.0): every airframe in range with a registration, and
// the airport each arrival came from (a gold banner for a new one).
static void noteCollection(const std::vector<StateVector> &states, const std::vector<FlightInfo> &flights)
{
    const time_t now = time(nullptr);
    if (now < 1600000000) return;
    const uint16_t today = SeenLog::dayNumber(now);
    static uint32_t recent[40] = {};   // noted today, so not again every fetch
    static uint16_t recentDay = 0;
    static uint8_t next = 0;
    if (recentDay != today) { memset(recent, 0, sizeof(recent)); recentDay = today; }
    // New ones wait here and go to the file together, about once a minute
    // (9.0.2: one open of the file a batch, not one an aircraft, which in
    // the morning rush meant several every few seconds and a heap broken
    // up by LittleFS's buffers: three self-heal restarts on 26 Oct).
    struct Pending { uint32_t icao; char reg[8], type[5], op[4]; };
    static Pending pend[12];
    static uint8_t nPend = 0;
    static unsigned long lastFlushMs = 0;
    for (const StateVector &s : states)
    {
        if (!s.registration.length() || nPend >= 12) continue;
        const uint32_t icao = (uint32_t)strtoul(s.icao24.c_str(), nullptr, 16);
        if (!icao) continue;
        bool seen = false;
        for (uint32_t r : recent) if (r == icao) { seen = true; break; }
        if (seen) continue;
        recent[next] = icao;
        next = (next + 1) % 40;
        Pending &p = pend[nPend++];
        p.icao = icao;
        strlcpy(p.reg, s.registration.c_str(), sizeof(p.reg));
        strlcpy(p.type, s.aircraft_type.c_str(), sizeof(p.type));
        p.op[0] = 0;
        if (s.callsign.length() >= 4 && isalpha((unsigned char)s.callsign[0]) && isalpha((unsigned char)s.callsign[1]) &&
            isalpha((unsigned char)s.callsign[2]) && isdigit((unsigned char)s.callsign[3]))
            strlcpy(p.op, s.callsign.c_str(), sizeof(p.op));
        // A special livery, the first time today (9.0).
        char livery[40];
        if (Liveries::find(s.registration.c_str(), livery, sizeof(livery)))
        {
            Log.printf("Spotter: special livery %s (%s) in range\n", s.registration.c_str(), livery);
            char l2[24];
            strlcpy(l2, livery, sizeof(l2));
            g_display.showMood(4, "Special livery", l2);
            Notify::post(Notify::Rare, 4, "rainbow", "Special livery", "%s (%s, %s) is in range%s%s.", livery,
                         s.registration.c_str(), s.aircraft_type.length() ? s.aircraft_type.c_str() : "?",
                         s.callsign.length() ? " as " : "", s.callsign.c_str());
        }
    }
    if (nPend && (nPend >= 12 || millis() - lastFlushMs > 60000UL))
    {
        lastFlushMs = millis();
        for (uint8_t i = 0; i < nPend; ++i)
            Airframes::visit(Airframes::fileStore(), pend[i].icao, today, pend[i].reg, pend[i].type, pend[i].op, false);
        Airframes::fileStore().done();
        nPend = 0;
    }
    for (const FlightInfo &f : flights)
    {
        if (f.destination.code_icao != g_airport.icao || f.origin.code_icao.length() != 4) continue;
        const int news = Logbook::noteOrigin(f.origin.code_icao.c_str(), f.ident.c_str());
        if (news == 2)
        {
            char iso[3];
            const char *country = "";
            Countries::of(f.origin.code_icao.c_str(), iso, &country);
            Log.printf("Logbook: a new country, %s (%s)\n", country, f.ident.c_str());
            g_display.showMood(5, "New sticker", country);   // a whole new country (9.1)
        }
        else if (news == 1)
        {
            const char *city = Airports::city(f.origin.code_icao.c_str());
            char l2[24];
            snprintf(l2, sizeof(l2), "%s", city && city[0] ? city : f.origin.code_icao.c_str());
            g_display.showNews("First arrival from", l2);
        }
    }
    // An airline the collection has never had (9.1).
    char code[4];
    while (g_display.takeNewAirline(code, sizeof(code)))
        if (Logbook::stickerIsNew(0, code))
        {
            const char *name = AirlineNames::of(code);
            Log.printf("Logbook: a new airline, %s\n", code);
            g_display.showMood(5, "New sticker", name ? name : code);
        }
}

static void spotterCheck(const std::vector<StateVector> &states, const std::vector<FlightInfo> &flights)
{
    const RuntimeConfig cfg = configSnapshot();
    Spotter::Settings set;
    set.home_lat = cfg.home_lat;
    set.home_lon = cfg.home_lon;
    set.overhead_km = cfg.overhead_km;
    set.overhead_all = cfg.overhead_all;
    set.watchlist = cfg.watchlist;
    Spotter::Event ev;
    if (!Spotter::check(states, set, millis(), ev)) return;
    const StateVector &s = states[ev.index];

    // Where it's going: from the cards' routes, else one adsb.im lookup (plain HTTP).
    FlightInfo route;
    bool haveRoute = false;
    for (const FlightInfo &f : flights)
        if (f.ident == s.callsign) { route = f; haveRoute = true; break; }
    if (!haveRoute && s.callsign.length() >= 4 && ESP.getMaxAllocHeap() > 20000)
    haveRoute = g_adsbIm.fetchFlightInfo(s.callsign, s.icao24, route) && route.destination.code_icao.length();   // plain HTTP

    OverheadScene::Info info;
    if (ev.watch)
    {
        strlcpy(info.label, "WATCHLIST", sizeof(info.label));
        info.labelColour = Rgb{255, 215, 80};
    }
    const String number = flightNumberFromCallsign(s.callsign);
    const String ident = number.length() ? number : s.callsign.length() ? s.callsign
                       : s.registration.length() ? s.registration : s.icao24;
    strlcpy(info.ident, ident.c_str(), sizeof(info.ident));
    strlcpy(info.type, s.aircraft_type.c_str(), sizeof(info.type));
    if (haveRoute)
    {
        const bool home = AirportPack::isHome(route.destination);
        const AirportInfo &end = home ? route.origin : route.destination;
        const char *city = cfg.route_cities ? Airports::city(end) : nullptr;
        const String code = end.code_iata.length() ? end.code_iata : end.code_icao;
        snprintf(info.where, sizeof(info.where), "%s %s", home ? "From" : "To", city ? city : code.c_str());
    }
    info.altFt = isnan(s.baro_altitude) ? NAN : (float)(s.baro_altitude * 3.28084);
    info.kind = AircraftSprites::classify(s.aircraft_type, info.known);
    FlightInfo who;
    who.ident = s.callsign;
    who.ident_icao = s.callsign;
    const String prefix = s.callsign.length() >= 3 ? s.callsign.substring(0, 3) : String();
    info.accent = prefix.length() ? AircraftSprites::airlineColour(prefix.c_str()) : Rgb{90, 170, 255};
    info.livery = AircraftSprites::liveryFor(who, info.accent);
    info.rightward = FlyAcross::pathFor(isnan(s.heading) ? 90 : s.heading, 0, cfg.screen_facing).rightward;
    Log.printf("Spotter: %s %s %s, %.0f ft, %.1f km from home%s%s\n", ev.watch ? "watchlist" : "overhead",
               info.ident, info.type, isnan(info.altFt) ? 0.0f : info.altFt, isnan(ev.distKm) ? 0.0f : ev.distKm,
               ev.watch ? ", matched " : "", ev.watch ? ev.matched : "");
    g_display.showSpot(info);
    snprintf(g_ha.spotted, sizeof(g_ha.spotted), "%s %s%s%s", info.ident, info.type, info.where[0] ? " " : "", info.where);
    if (ev.watch)
        Notify::post(Notify::Watch, 4, "eyes", "Watchlist", "%s %s%s%s (%s) at %.0f ft%s%s",
                     info.ident, info.type, s.registration.length() ? " " : "", s.registration.c_str(),
                     ev.matched, isnan(info.altFt) ? 0.0f : info.altFt, info.where[0] ? ", " : "", info.where);
}

// Save today's stats if they have changed: ~7 KB in one block, only when
// there is one. The feed's connection is never closed for it: once closed,
// a fragmented heap can leave no room to reopen it (5.1's soak sat without
// positions for 25 minutes that way); a skipped save tries again in 5 minutes.
// The airfield's mood and the runway outlook (8.2), after each weather
// report: what the weather screen and web page say, and a banner and a
// phone alert when fog or a storm sets in, or a runway change is due
// within three hours.
static void airfieldCheck()
{
    const time_t now = time(nullptr);
    // Mood: announced as it sets in, each kind at most every three hours
    // (a report either side of the line would otherwise flap).
    const uint8_t mood = Weather::mood(g_metar, (Weather::Mood)g_mood);
    if (mood != g_mood)
    {
        Log.printf("Weather: mood %u -> %u (%s)\n", (unsigned)g_mood, (unsigned)mood, g_metar.raw);
        g_mood = mood;
        static unsigned long announcedMs[4] = {};
        if (mood != Weather::Calm && (!announcedMs[mood] || millis() - announcedMs[mood] > 3UL * 3600 * 1000))
        {
            announcedMs[mood] = millis();
            char l1[24], l2[28], what[24];
            Weather::moodLines((Weather::Mood)mood, g_metar, l1, sizeof(l1), l2, sizeof(l2));
            g_display.showMood(mood == Weather::LowVis ? 1 : 2, l1, l2);
            Weather::describe(g_metar.wx, what, sizeof(what));
            char extra[24] = "";
            if (mood == Weather::LowVis)
            {
                if (g_metar.ceilingFt >= 0 && g_metar.visM >= 0 && g_metar.visM < 1500)
                    snprintf(extra, sizeof(extra), ", cloud at %d ft", g_metar.ceilingFt);
                Notify::post(Notify::Weather, 3, "fog", l1, "%s%s. Expect wider gaps between arrivals.", l2, extra);
            }
            else
            {
                if (g_metar.gustKt) snprintf(extra, sizeof(extra), " gusting %d", g_metar.gustKt);
                Notify::post(Notify::Weather, 4, "zap", l1, "%s: %s, wind %03d/%d%s.", l2, what[0] ? what : "thunder",
                             g_metar.windDir < 0 ? 0 : g_metar.windDir, g_metar.windKt, extra);
            }
        }
    }

    static time_t tafIssued = 0;
    if (g_taf.valid && g_taf.issued != tafIssued)
    {
        tafIssued = g_taf.issued;
        struct tm u;
        gmtime_r(&g_taf.to, &u);
        Log.printf("Weather: TAF read, %u winds to %02d %02d:00Z\n", (unsigned)g_taf.n, u.tm_mday, u.tm_hour);
    }

    // The landing course in use (the last one seen, overnight too).
    static float course = NAN;
    {
        char arr[12], dep[12];
        g_display.runwaysInUse(arr, sizeof(arr), dep, sizeof(dep));
        char *sp = strchr(arr, ' ');
        if (sp) *sp = 0;
        for (int i = 0; arr[0] && i < g_airport.runwayCount; ++i)
            if (!strcmp(g_airport.runways[i].name, arr)) course = g_airport.runways[i].course;
    }
    Weather::Outlook o;
    char shortText[20] = "", longText[48] = "";
    static char lastTo[4] = "";
    static time_t lastAt = 0;
    if (Weather::runwayOutlook(g_taf, course, now, o) && o.change)
    {
        struct tm lt, today;
        localtime_r(&o.at, &lt);
        localtime_r(&now, &today);
        if (o.at <= now + 600)                 snprintf(shortText, sizeof(shortText), "To %s due now", o.to);
        else if (lt.tm_yday == today.tm_yday)  snprintf(shortText, sizeof(shortText), "To %s at %02d:%02d", o.to, lt.tm_hour, lt.tm_min);
        else                                   snprintf(shortText, sizeof(shortText), "To %s tmrw %02d:%02d", o.to, lt.tm_hour, lt.tm_min);
        snprintf(longText, sizeof(longText), "Runway %s %s%02d:%02d (wind %03d/%d)", o.to,
                 o.at <= now + 600 ? "due now, " : lt.tm_yday == today.tm_yday ? "from about " : "tomorrow from ",
                 lt.tm_hour, lt.tm_min, o.windDir < 0 ? 0 : o.windDir, o.windKt);
        if (o.at <= now + 600) snprintf(longText, sizeof(longText), "Runway %s due now (wind %03d/%d)", o.to,
                                        o.windDir < 0 ? 0 : o.windDir, o.windKt);
        // A change within three hours: announce it once (again only if the
        // forecast moves it by more than 90 minutes, or to another runway).
        const bool soon = o.at - now <= 3 * 3600;
        const bool news = strcmp(o.to, lastTo) != 0 || (o.at > now + 600 && labs((long)(o.at - lastAt)) > 5400);
        if (soon && news)
        {
            strlcpy(lastTo, o.to, sizeof(lastTo));
            lastAt = o.at;
            char l2[24];
            if (o.at <= now + 600) snprintf(l2, sizeof(l2), "To %s soon", o.to);
            else                   snprintf(l2, sizeof(l2), "To %s at %02d:%02d", o.to, lt.tm_hour, lt.tm_min);
            g_display.showMood(3, "Runway change due", l2);
            Notify::post(Notify::Runway, 3, "arrows_counterclockwise", "Runway change expected",
                         "The forecast wind (%03d/%d kt) should turn %s's arrivals onto %s %s %02d:%02d.",
                         o.windDir < 0 ? 0 : o.windDir, o.windKt, g_airport.name, o.to,
                         o.at <= now + 600 ? "any time from" : "around", lt.tm_hour, lt.tm_min);
            Log.printf("Weather: runway outlook: %s\n", longText);
        }
    }
    else lastTo[0] = 0;
    static const char *const kMoodNames[] = {"", "Low visibility", "Thunderstorm", "Thunder nearby"};
    g_display.setAirfield(g_mood, shortText);
    g_webConfig.setAirfield(kMoodNames[g_mood & 3], longText);
}

// The year so far (9.0), for the panel's year pages: after each day is
// logged, and at the start.
static void refreshYear()
{
    const time_t now = time(nullptr);
    if (now < 1600000000) return;
    struct tm lt;
    localtime_r(&now, &lt);
    // In the first days of January it is still last year's that's shown.
    const uint16_t yr = lt.tm_mon == 0 && lt.tm_mday <= 2 ? lt.tm_year + 1899 : lt.tm_year + 1900;
    Logbook::Year y;
    Logbook::year(yr, y);
    g_display.setYear(y);
    Log.printf("Logbook: %u so far: %u days, %u movements\n", (unsigned)yr, y.days, (unsigned)(y.arrivals + y.departures));
}

static void saveStats()
{
    StageTrace::mark(StageTrace::Main, StageTrace::MainStats);
    const size_t need = g_display.statsSize();
    if (!need) return;
    if (ESP.getMaxAllocHeap() < need + 4096)
    {
        Log.println("Stats: no room to save now, trying again in 5 minutes");
        return;
    }
    std::vector<uint8_t> data;
    if (g_display.statsSnapshot(data) && !DailyStats::writeFile(data))
        Log.println("Stats: could not save");
}

static void checkSquawks(const std::vector<StateVector> &states)
{
    static std::map<String, int>           streak;    // icao24+code -> fetches in a row
    static std::map<String, unsigned long> alerted;   // icao24+code -> when alerted
    const unsigned long now = millis();
    std::map<String, int> seen;
    for (const StateVector &s : states)
    {
        const char *meaning = squawkMeaning(s.squawk);
        if (!meaning) continue;
        const String key = s.icao24 + s.squawk;
        seen[key] = streak.count(key) ? streak[key] + 1 : 1;
        if (seen[key] < 2) continue;
        auto a = alerted.find(key);
        if (a != alerted.end() && now - a->second < 30UL * 60 * 1000) continue;
        alerted[key] = now;

        // "4200FT 12KM E": altitude, distance and direction from the centre.
        char detail[24];
        static const char *const kDirs[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
        const int dir = isnan(s.bearing_deg) ? -1 : ((int)lround(s.bearing_deg / 45.0)) % 8;
        snprintf(detail, sizeof(detail), "%.0fFT %.0fKM %s",
                 isnan(s.baro_altitude) ? 0.0 : s.baro_altitude * 3.28084,
                 isnan(s.distance_km) ? 0.0 : s.distance_km, dir >= 0 ? kDirs[dir] : "");
        const String ident = s.callsign.length() ? s.callsign : s.icao24;
        Log.printf("SQUAWK %s (%s): %s, %s\n", s.squawk.c_str(), meaning, ident.c_str(), detail);
        g_display.raiseAlert(s.squawk.c_str(), meaning, ident.c_str(), detail);
        Notify::post(Notify::Emergency, 5, "rotating_light", "Emergency squawk",
                     "%s squawking %s (%s), %s", ident.c_str(), s.squawk.c_str(), meaning, detail);
    }
    streak.swap(seen);
    for (auto it = alerted.begin(); it != alerted.end(); )
        it = now - it->second > 60UL * 60 * 1000 ? alerted.erase(it) : std::next(it);
}

// Map dot colour per airline (ICAO prefix of the call sign): the logo's accent,
// worked out once per airline. Unknown airlines and GA get a neutral white.
static Rgb airlineColour(const String &callsign)
{
    // A fixed table (8.0): a map's node per new airline, all day, scattered
    // small permanent blocks through the heap.
    // 76 airlines, the oldest replaced when full (8.3.1: 64 and then no
    // caching at all, so a busy day's 65th airline onwards was worked out
    // again on every fetch).
    struct Slot { char icao[3]; Rgb c; };   // 6 bytes: no terminator kept
    static Slot cache[76];
    static uint8_t cached = 0, next = 0;
    const Rgb kNeutral{200, 205, 215};
    if (callsign.length() < 4 || !isalpha((unsigned char)callsign[0]) ||
        !isalpha((unsigned char)callsign[1]) || !isalpha((unsigned char)callsign[2]) ||
        !isdigit((unsigned char)callsign[3]))
        return kNeutral;
    String icao = callsign.substring(0, 3);
    icao.toUpperCase();
    static uint32_t logosVersion = 0;
    if (logosVersion != LogoBook::version()) { cached = 0; logosVersion = LogoBook::version(); }   // a logo was added
    for (uint8_t i = 0; i < cached; ++i)
        if (strncmp(cache[i].icao, icao.c_str(), 3) == 0) return cache[i].c;
    // From the logo's file a piece at a time (8.3.1): loading the 2 KB logo
    // to work it out crashed the board when the heap had no 2 KB block.
    Rgb c = kNeutral;
    if (!g_logoStore.accentOf(icao.c_str(), c))
    {
        c = kNeutral;
        LogoBook::noteMissing(icao);   // for the web page's Logos section
    }
    Slot &slot = cached < 76 ? cache[cached++] : cache[next++ % 76];
    memcpy(slot.icao, icao.c_str(), 3);
    slot.c = c;
    return c;
}

// Every aircraft in range as a map point: position, track, speed, airline
// colour, and whether it is lined up on final for a Heathrow runway.
static std::vector<TrafficPoint> trafficFromStates(const std::vector<StateVector> &states)
{
    // Sized exactly, and gone after the fetch: reserving room for 48 for
    // good (5.1 candidate) cost 6 KB of heap that the feed's fetches need.
    std::vector<TrafficPoint> pts;
    pts.reserve(min(states.size(), (size_t)TrafficTracker::kMax));
    for (const StateVector &s : states)
    {
        TrafficPoint p;
        p.id      = (uint32_t)strtoul(s.icao24.c_str(), nullptr, 16);
        p.lat     = s.lat;
        p.lon     = s.lon;
        p.heading = s.heading;
        p.gsKt    = isnan(s.velocity) ? NAN : s.velocity * 1.94384;
        p.colour  = squawkMeaning(s.squawk) ? Rgb{255, 40, 30} : airlineColour(s.callsign);
        {
            String cs = s.callsign;
            cs.trim();
            const String num = flightNumberFromCallsign(cs);
            String label = num.length() ? num : cs.length() ? cs : s.registration;
            label.replace(" ", "");   // "U2 73CA" -> "U273CA": the tiny font has no room for gaps
            strlcpy(p.label, label.c_str(), sizeof(p.label));
            strlcpy(p.type, s.aircraft_type.c_str(), sizeof(p.type));
            p.size = AircraftSprites::sizeClass(p.type);
            p.altFt = isnan(s.baro_altitude) ? NAN : (float)(s.baro_altitude * 3.28084);
        }

        FlightInfo f;   // just enough for the approach model
        f.lat = s.lat;  f.lon = s.lon;  f.heading = s.heading;
        f.baro_altitude = isnan(s.baro_altitude) ? NAN : s.baro_altitude * 3.28084;
        f.velocity      = p.gsKt;
        f.vertical_rate = isnan(s.vertical_rate) ? NAN : s.vertical_rate * 196.85;
        const ApproachStatus st = ApproachModel::evaluate(f);
        p.onFinal = st.phase == ApproachStatus::Approach;
        // For the approach line-up: on final (or just landing) within 25 km.
        if ((st.phase == ApproachStatus::Approach || st.phase == ApproachStatus::Landing) &&
            !isnan(st.distKm) && st.distKm <= 25.4f)
        {
            p.finalKm10 = (uint8_t)lroundf(st.distKm * 10);
            strlcpy(p.rwy, st.runway, sizeof(p.rwy));
        }
        if (pts.size() >= (size_t)TrafficTracker::kMax) break;   // never past the reservation
        pts.push_back(p);
    }
    return pts;
}

// Arrivals board from every aircraft in range, not just the enriched ones
// (with "nearest only" just one flight is looked up per fetch). On final the
// ETA comes from the approach model. Further out, an aircraft counts as
// inbound if its route says so or, without a route, if it is below 10,000 ft,
// descending and pointing at Heathrow; its ETA is a rough guess.
static void updateArrivals(const std::vector<StateVector> &states, const std::vector<FlightInfo> &flights)
{
    const double kLhrLat = g_airport.lat, kLhrLon = g_airport.lon;
    InfoScreens::Arrival rows[16];
    int n = 0;
    for (const StateVector &s : states)
    {
        if (n >= 16) break;
        const FlightInfo *known = nullptr;
        for (const FlightInfo &f : flights)
            if (f.ident == s.callsign) { known = &f; break; }

        FlightInfo f;
        if (known) { f.origin = known->origin; f.destination = known->destination; }
        f.lat = s.lat;  f.lon = s.lon;  f.heading = s.heading;
        f.baro_altitude = isnan(s.baro_altitude) ? NAN : s.baro_altitude * 3.28084;
        f.velocity      = isnan(s.velocity) ? NAN : s.velocity * 1.94384;
        f.vertical_rate = isnan(s.vertical_rate) ? NAN : s.vertical_rate * 196.85;
        const ApproachStatus st = ApproachModel::evaluate(f);
        const bool onFinal = st.phase == ApproachStatus::Approach || st.phase == ApproachStatus::Landing;

        bool inbound = st.phase == ApproachStatus::Inbound;
        if (!onFinal && !inbound && !known && !isnan(f.baro_altitude) && f.baro_altitude < 10000 &&
            !isnan(f.vertical_rate) && f.vertical_rate < -300 && !isnan(f.heading))
        {
            const double toLhr = computeBearingDeg(s.lat, s.lon, kLhrLat, kLhrLon);
            const double off = fabs(fmod(f.heading - toLhr + 540.0, 360.0) - 180.0);
            inbound = off < 60;
        }
        if (!onFinal && !inbound) continue;

        InfoScreens::Arrival &a = rows[n];
        a = InfoScreens::Arrival();
        String id = known && known->ident_iata.length() ? known->ident_iata : flightNumberFromCallsign(s.callsign);
        if (!id.length()) id = s.callsign.length() ? s.callsign : s.icao24;
        strlcpy(a.ident, id.c_str(), sizeof(a.ident));
        const String &type = known && known->aircraft_code.length() ? known->aircraft_code : s.aircraft_type;
        strlcpy(a.type, type.c_str(), sizeof(a.type));
        if (known)
            strlcpy(a.from, (known->origin.code_iata.length() ? known->origin.code_iata : known->origin.code_icao).c_str(), sizeof(a.from));
        a.accent = airlineColour(s.callsign);
        a.dataMs = millis();
        if (onFinal)
            a.etaSec = st.etaSec;
        else
        {
            a.estimate = true;
            const double km = haversineKm(s.lat, s.lon, kLhrLat, kLhrLon);
            const float gs = isnan(f.velocity) || f.velocity < 120 ? 200.0f : (float)f.velocity;
            a.etaSec = km * 1000.0f / (gs * 0.514444f) * 1.3f + 120.0f;
        }
        if (isnan(a.etaSec)) continue;
        ++n;
    }
    std::sort(rows, rows + n, [](const InfoScreens::Arrival &x, const InfoScreens::Arrival &y) {
        return x.etaSec < y.etaSec;
    });
    // The weather fetch asks for where the next few came from as well (8.0).
    {
        char ids[sizeof(g_originIds)] = "";
        int k = 0;
        for (int i = 0; i < n && k < 4; ++i)
        {
            Airports::Place p;
            const size_t len = strlen(rows[i].from);
            if (!((len == 3 && Airports::byIata(rows[i].from, p)) || (len == 4 && Airports::byIcao(rows[i].from, p)))) continue;
            if (strcmp(p.icao, g_airport.icao) == 0 || strstr(ids, p.icao)) continue;
            if (k++) strlcat(ids, ",", sizeof(ids));
            strlcat(ids, p.icao, sizeof(ids));
        }
        strlcpy(g_originIds, ids, sizeof(g_originIds));
    }
    g_display.setArrivals(rows, n);
    strlcpy(g_ha.next, n ? rows[0].ident : "", sizeof(g_ha.next));
    g_ha.nextEtaSec = n ? (int)lroundf(rows[0].etaSec) : -1;
}

// Recent departures (newest first) for the board's second page: every
// aircraft climbing out along a runway, its destination if a route is known.
static void updateDepartures(const std::vector<StateVector> &states, const std::vector<FlightInfo> &flights)
{
    static InfoScreens::Departure recent[InfoScreens::kMaxDepartures];
    static uint32_t ids[InfoScreens::kMaxDepartures] = {};
    static int n = 0;
    bool changed = false;
    for (const StateVector &s : states)
    {
        FlightInfo f;
        f.lat = s.lat;  f.lon = s.lon;  f.heading = s.heading;
        f.baro_altitude = isnan(s.baro_altitude) ? NAN : s.baro_altitude * 3.28084;
        f.velocity      = isnan(s.velocity) ? NAN : s.velocity * 1.94384;
        f.vertical_rate = isnan(s.vertical_rate) ? NAN : s.vertical_rate * 196.85;
        char rwy[4] = "";
        if (!ApproachModel::climbingOut(f, rwy, sizeof(rwy))) continue;
        const uint32_t id = (uint32_t)strtoul(s.icao24.c_str(), nullptr, 16) | 1;
        bool seen = false;
        for (int i = 0; i < n; ++i) if (ids[i] == id) seen = true;
        if (seen) continue;

        const FlightInfo *known = nullptr;
        for (const FlightInfo &k : flights)
            if (k.ident == s.callsign) { known = &k; break; }
        InfoScreens::Departure d;
        String id2 = known && known->ident_iata.length() ? known->ident_iata : flightNumberFromCallsign(s.callsign);
        if (!id2.length()) id2 = s.callsign.length() ? s.callsign : s.icao24;
        strlcpy(d.ident, id2.c_str(), sizeof(d.ident));
        strlcpy(d.type, (known && known->aircraft_code.length() ? known->aircraft_code : s.aircraft_type).c_str(), sizeof(d.type));
        if (known)
            strlcpy(d.dest, (known->destination.code_iata.length() ? known->destination.code_iata
                                                                   : known->destination.code_icao).c_str(), sizeof(d.dest));
        strlcpy(d.runway, rwy, sizeof(d.runway));
        d.accent = airlineColour(s.callsign);
        const time_t t = time(nullptr);
        d.at = t > 1600000000 ? t : 0;
        for (int i = min(n, InfoScreens::kMaxDepartures - 1); i > 0; --i) { recent[i] = recent[i - 1]; ids[i] = ids[i - 1]; }
        recent[0] = d;
        ids[0] = id;
        n = min(n + 1, InfoScreens::kMaxDepartures);
        changed = true;
    }
    if (changed) g_display.setDepartures(recent, n);
}

void setup()
{
    TlsArena::install();   // before any TLS: its big buffers get their own room
    Serial.begin(115200);
#if ARDUINO_USB_MODE && ARDUINO_USB_CDC_ON_BOOT
    // Native USB serial: with nothing reading the port, the default 100 ms
    // timeout stalls every write. 1 ms makes HWCDC give up after one stalled
    // write and drop output until the host reads again. (0 must NOT be used:
    // HWCDC's retry counter starts at the timeout and is decremented, so 0
    // wraps and a write blocks practically forever.)
    Serial.setTxTimeoutMs(1);
#endif
    delay(200);

    // A hung display or web task reboots the board after 30 s instead of
    // leaving a frozen panel; the reason (and where each task was, via
    // StageTrace) is logged on the next boot.
    esp_task_wdt_init(30, true);
    StageTrace::reportAtBoot();
    Health::begin();
    ImageCheck::begin();

    // Load config first so g_config is fully populated before any component reads it.
    // initialize() calls setBrightness8(g_config.display_brightness) — must be non-zero.
    loadConfig();

    // Reconfigure the flight-fetcher chain based on the saved priority setting.
    // Default:          hexdb → AeroAPI → OpenSky
    // opensky_priority: OpenSky → AeroAPI → hexdb
    if (g_config.opensky_priority)
    {
        g_aeroApiFallback.setPrimary(&g_aeroApi);
        g_aeroApiFallback.setSecondary(&g_hexDb);
        g_flightFetcher.setPrimary(&g_openSkyRoute);
        g_flightFetcher.setSecondary(&g_aeroApiFallback);
    }

    g_display.initialize();
    g_display.displayMessage(String("Glideslope"));

    // Mount LittleFS for local logo storage. Failure is non-fatal.
    g_logoStore.initialize();
    g_display.setLogoStore(&g_logoStore);
    RareSpotter::begin();   // type log for "first sighting" (on the same LittleFS)
    Logbook::begin();       // the days, events and collection kept for good (8.1)
    LogoBook::loadMissing(); // airlines seen without a logo, kept across restarts (8.1)
    // 9.0: the route book of 8.3-8.4 could have learned routes adsb.im
    // marked implausible; it was two days old, so start it again once.
    {
        Preferences p;
        if (p.begin("routebook", false))
        {
            if (!p.isKey("v90")) { LittleFS.remove("/routes.txt"); LittleFS.remove("/routes.old"); p.putBool("v90", true); }
            p.end();
        }
    }
    g_display.setRecords(Logbook::records());
    AirportPack::load();    // Heathrow, or the airport pack on LittleFS
    g_display.loadStats();  // today's tally survives a restart

    // Resolve WiFi credentials: NVS-saved (provisioner) overrides compile-time constants.
    // If neither has an SSID, launch the captive-portal AP so the user can configure.
    g_wifiSsid = String(WiFiConfiguration::WIFI_SSID);
    g_wifiPass = String(WiFiConfiguration::WIFI_PASSWORD);
    {
        String savedSsid, savedPass;
        if (WifiProvisioner::loadCredentials(savedSsid, savedPass))
        {
            g_wifiSsid = savedSsid;
            g_wifiPass = savedPass;
        }
    }
    if (g_wifiSsid.length() == 0)
    {
        g_display.displayMessage("WiFi Setup");
        delay(800);
        WifiProvisioner provisioner;
        provisioner.setStatusCallback([](const char *m) { g_display.displayMessage(m); });
        provisioner.run("Glideslope-Setup");
        // run() calls ESP.restart() — execution never reaches here
    }

    if (g_wifiSsid.length() > 0)
    {
        WiFi.mode(WIFI_STA);
        // Modem sleep saves little here (the board runs off mains) and is a
        // common cause of ESP32 links that go silently dead.
        WiFi.setSleep(false);
        WiFi.setAutoReconnect(true);
        g_display.displayMessage(String("WiFi: ") + g_wifiSsid);
        WiFi.begin(g_wifiSsid.c_str(), g_wifiPass.c_str());
        Log.print("Connecting to WiFi");
        int attempts = 0;
        while (WiFi.status() != WL_CONNECTED && attempts < 50)
        {
            delay(200);
            Log.print(".");
            attempts++;
        }
        Log.println();
        if (WiFi.status() == WL_CONNECTED)
        {
            Log.printf("WiFi connected: %s\n", WiFi.localIP().toString().c_str());

            // Synchronise the clock for night-mode scheduling. SNTP syncs in the
            // background; the TZ rule makes localtime() follow GMT/BST.
            configTzTime(g_airport.tz, "pool.ntp.org", "time.nist.gov");
            Log.println("NTP sync started (pool.ntp.org)");
            // A one-tap update asked for before the restart (8.2): now, with
            // the most memory the board will have. On success it restarts
            // into the new firmware; otherwise it carries on starting up.
            {
                char tag[16];
                if (Ota::takeBootRequest(tag, sizeof(tag))) Ota::installRelease(tag);
            }

            g_webConfig.setTodaySource([](String &out) { g_display.todayJson(out); });
            g_webConfig.setTrafficSource([](String &out, bool withAirport) { g_display.trafficJson(out, withAirport); });
            g_webConfig.begin(80);  // start HTTP config + log UI

            // http://glideslope.local/ as well as the IP address.
            if (MDNS.begin("glideslope"))
            {
                MDNS.addService("http", "tcp", 80);
                Log.println("mDNS: http://glideslope.local/");
            }

            // Run the web server on a dedicated FreeRTOS task pinned to the
            // application core (core 1, same as loop()).  Single-core scheduling
            // means no true parallelism — no data-race risk — but the web task
            // gets CPU during every blocking socket/HTTP operation in the main loop,
            // keeping the UI responsive throughout the AeroAPI fetch cycle.
            xTaskCreatePinnedToCore(
                [](void *) {
                    esp_task_wdt_add(nullptr);   // watched like the display task
                    for (;;) { g_webConfig.loop(); esp_task_wdt_reset(); delay(5); }
                },
                "webcfg",
                8192,       // stack: HTML send + ArduinoJson + log build
                nullptr,
                1,          // same priority as loop()
                &g_webTask,
                APP_CPU_NUM // core 1
            );

            g_display.displayMessage(String("WiFi OK ") + WiFi.localIP().toString());
            delay(1000);
            g_display.displayMessage("glideslope.local");
            delay(1500);
            g_display.showLoading();
        }
        else
        {
            Log.println("WiFi not connected; proceeding without network");
            g_display.displayMessage(String("WiFi FAIL"));
        }
    }

    // hexdb.io is always the primary enrichment source (free, no key required).
    // AeroAPI is used only as a fallback when hexdb has no route for a callsign,
    // and only when a key has been saved via the web config UI.
    if (g_config.opensky_priority)
        Log.printf("Flight enrichment: OpenSky primary%s + hexdb fallback\n",
                   strlen(g_config.aeroapi_key) > 0 ? " + AeroAPI" : "");
    else
        Log.printf("Flight enrichment: hexdb.io primary%s%s\n",
                   strlen(g_config.aeroapi_key)       > 0 ? " + AeroAPI fallback"       : "",
                   strlen(g_config.opensky_client_id) > 0 ? " + OpenSky route fallback" : "");
    Log.println("Flight enrichment: adsb.im routes first (plain HTTP)");
    g_fetcher = new FlightDataFetcher(&g_stateFetcher, &g_routes, &g_logoStore);

    // Force the first fetch to fire on the very first loop() iteration rather
    // than waiting a full FETCH_INTERVAL_SECONDS from boot.
    // Unsigned wraparound is intentional — now - g_lastFetchMs will equal
    // exactly FETCH_INTERVAL_SECONDS * 1000 on the first call.
    g_lastFetchMs = millis() -
        (unsigned long)(g_config.fetch_interval_seconds * 1000UL);

    // From here the display task owns the panel (animation keeps running
    // while loop() blocks on network fetches).
    g_display.startTask();
    Notify::setFrame(&shownFrame());   // the panel goes with emergency, rare-spot and watchlist alerts
}

// Reconnect WiFi if the connection has dropped.
// Returns true when connected (either was already up or just recovered).
static bool ensureWiFi()
{
    if (WiFi.status() == WL_CONNECTED)
        return true;

    if (g_wifiSsid.length() == 0)
        return false;

    Log.println("WiFi lost — reconnecting...");
    g_display.displayMessage("WiFi...");

    WiFi.disconnect(true);
    delay(200);
    WiFi.begin(g_wifiSsid.c_str(), g_wifiPass.c_str());

    for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; ++i)
        delay(500);

    if (WiFi.status() == WL_CONNECTED)
    {
        Log.printf("WiFi reconnected: %s\n", WiFi.localIP().toString().c_str());
        g_display.displayMessage("WiFi OK");
        delay(1000);
        return true;
    }

    Log.println("WiFi reconnect failed");
    g_display.displayMessage("WiFi FAIL");
    return false;
}

void loop()
{
    // Fast interval while a live source answers (local receiver or the
    // community feeds); the slower, quota-limited one when it fell to OpenSky.
    const bool onOpenSky = g_stateFetcher.usedFallback() && g_feedChain.usedFallback();
    const bool onFeeds   = g_stateFetcher.usedFallback() && !g_feedChain.usedFallback();
    const unsigned long intervalMs = onFeeds
        ? max(g_config.local_fetch_interval_seconds * 1000UL, g_feeds.minIntervalMs())
        : onOpenSky
        ? g_config.fetch_interval_seconds       * 1000UL
        : g_config.local_fetch_interval_seconds * 1000UL;
    const unsigned long now = millis();

    // Night-mode transition check — runs every loop() tick, independently of the
    // fetch timer.  When the device wakes from a screen-off night period, reset the
    // fetch timer so the very next timer check fires immediately rather than waiting
    // up to a full fetch_interval_seconds.
    const bool screenOffAtNight = isNightActive() && g_config.night_brightness == 0;
    if (screenOffAtNight && !g_wasNightSuppressed)
    {
        Log.println("Night mode — API calls suppressed (screen off)");
        g_wasNightSuppressed = true;
    }
    else if (!screenOffAtNight && g_wasNightSuppressed)
    {
        Log.println("Night mode ended — queuing immediate fetch");
        g_wasNightSuppressed = false;
        // Back-date the timer so the fetch block below fires this iteration
        g_lastFetchMs = now - intervalMs;
    }

    // adsb.lol resting after a refusal: wait it out (the display dead-reckons)
    // rather than switch to adsb.fi and its 65 KB TLS handshake.
    if (onFeeds && now - g_lastFetchMs >= intervalMs && g_config.use_community_feeds)
    {
        const unsigned long hold = g_feeds.holdOffMs();
        if (hold) g_lastFetchMs = now - intervalMs + min(hold, 30000UL);
    }

    if (g_display.showcaseRunning())
    {
        // The showcase owns the panel (and may be recorded): no fetches, no
        // web pauses, no real flights cutting in.
    }
    else if (now - g_lastFetchMs >= intervalMs)
    {
        g_lastFetchMs = now;

        if (screenOffAtNight)
        {
            // Nothing to do — already logged the suppression above.
        }
        else if (g_updating)
        {
            // A firmware upload has the board: no fetch, and the feed's TLS
            // session closed for the memory (it reopens on the next fetch).
            if (g_tlsReleaser) g_tlsReleaser();
            g_feedPaused = true;
        }
        else if (!ensureWiFi())
        {
            Log.println("Skipping fetch — no WiFi");
        }
        else if (stackDue(now))
        {
            // Holding stacks: every 45 s one fix's airspace takes this fetch
            // slot, so adsb.lol sees no extra requests (each fix every 3 min).
            sampleStack();
        }
        else
        {
        size_t enriched;
        {
            // 9.1: no NetBusy round the whole fetch (it kept the web server
            // waiting most of the time: a phone took 10-20 s to load the
            // page); each secure connection holds it while it is set up.
            StageTrace::mark(StageTrace::Main, StageTrace::MainFetch);
            // Out of memory part-way (a heap that tight is rare, but a
            // phone on the live preview once tipped it): skip this round,
            // keep the board up, and try again in 5 s.
            try
            {
                enriched = g_fetcher->fetchFlights(g_states, g_flights);
            }
            catch (const std::bad_alloc &)
            {
                g_states.clear();
                g_flights.clear();
                g_states.shrink_to_fit();
                g_flights.shrink_to_fit();
                enriched = 0;
                Log.printf("Heap: out of memory in the fetch (%u free, block %u); skipped\n",
                           (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
            }
            StageTrace::mark(StageTrace::Main, StageTrace::MainTraffic);
        }

        // Dead-link watchdog: the ESP32 can stay "connected" to Wi-Fi with a
        // link that passes nothing (every DNS lookup fails). After a run of
        // fetches where no source answered, reconnect; if that doesn't help
        // either, restart.
        {
            static int failStreak = 0;
            // A fetch skipped for lack of heap (tlsAffordable) is not a dead link.
            // (The adsb.lol connection holds ~40 KB, so "short" is well below that.)
            const bool memoryShort = ESP.getFreeHeap() < 26000;
            if (g_fetcher->lastFetchOk()) failStreak = 0;
            else if (!memoryShort)        ++failStreak;
            if (failStreak == 6 || failStreak == 12)
            {
                Log.printf("WiFi: %d fetches in a row failed, reconnecting\n", failStreak);
                WiFi.disconnect(false);
                delay(200);
                WiFi.begin(g_wifiSsid.c_str(), g_wifiPass.c_str());
                for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; ++i) delay(250);
            }
            else if (failStreak >= 18)
            {
                Log.println("WiFi: still no data after reconnecting, restarting");
                Health::noteRestart("self-heal: no data on Wi-Fi");
                Health::saveBlackBox("self-heal: no data on Wi-Fi");
                delay(200);
                ESP.restart();
            }
        }
        // No memory for the feed: its connection put off for want of heap
        // (too little free, or broken into pieces) for two minutes running.
        // It doesn't mend itself: save the day's stats and restart, a few
        // seconds instead of a board with no flights. (Until 9.1.3 this
        // looked at heap figures instead: free over 22 KB and the largest
        // block under 10 KB, which missed 20-30 KB free with 12 KB blocks,
        // and the board went without flights until a restart by hand.)
        {
            if (g_fetcher->lastFetchOk()) g_feedDeferredSinceMs = 0;
            if (g_feedDeferredSinceMs && millis() - g_feedDeferredSinceMs > 120000)
            {
                Log.printf("Heap: no memory for the feed for 2 minutes (%u free, largest block %u): restarting\n",
                           (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
                saveStats();
                ImageCheck::keep("restart to recover the feed's memory");
                Health::noteRestart("self-heal: no memory");   // (the list keeps 27 characters)
                Health::saveBlackBox("self-heal: no memory for the feed for 2 minutes");
                delay(300);
                ESP.restart();
            }
        }
        if (g_fetcher->lastFetchOk()) ImageCheck::keep("positions fetched");
        Health::noteFetch(g_fetcher->lastFetchOk());
        noteHistory(g_states, g_flights);       // "SEEN 14 TIMES" on the cards
        noteCollection(g_states, g_flights);    // every airframe, every origin (9.0)
        g_display.displayFlights(g_flights);   // queues new contacts, refreshes telemetry
        g_display.updateTraffic(trafficFromStates(g_states));   // everything in range, for the map
        heapCheckpoint("fetch cycle");
        checkSquawks(g_states);
        spotterCheck(g_states, g_flights);
        g_display.noteTraffic(g_states);
        {
            char arr[12], dep[12];
            g_display.runwaysInUse(arr, sizeof(arr), dep, sizeof(dep));
            g_webConfig.setRunways(arr, dep);
        }
        updateArrivals(g_states, g_flights);
        updateDepartures(g_states, g_flights);
        {
            g_display.runwaysInUse(g_ha.arrRunways, sizeof(g_ha.arrRunways), g_ha.depRunways, sizeof(g_ha.depRunways));
            g_display.todayCounts(g_ha.arrivals, g_ha.departures);
            char hold[8];
            g_ha.holding = g_display.holdingSummary(hold, sizeof(hold));
            FlightInfo cur;
            if (g_display.currentFlight(cur))
                snprintf(g_ha.last, sizeof(g_ha.last), "%s %s>%s",
                         (cur.ident_iata.length() ? cur.ident_iata : cur.ident).c_str(),
                         (cur.origin.code_iata.length() ? cur.origin.code_iata : cur.origin.code_icao).c_str(),
                         (cur.destination.code_iata.length() ? cur.destination.code_iata : cur.destination.code_icao).c_str());
        }

        static String lastIdents;
        String idents;
        for (const auto &f : g_flights) idents += f.ident + " ";
        const char *source = !g_stateFetcher.usedFallback() ? "local receiver"
                           : !g_feedChain.usedFallback()    ? g_feeds.lastSource()
                                                            : "OpenSky";
        if (idents != lastIdents)
        {
            lastIdents = idents;
            Log.printf("Fetch (%s): %d aircraft in range, showing: %s\n",
                       source, (int)g_states.size(), idents.length() ? idents.c_str() : "-");
            for (const auto &f : g_flights)
                Log.printf("  %s (%s) %s>%s %s, %.0f ft, %.0f kt, hdg %.0f\n",
                           f.ident_iata.c_str(), f.ident.c_str(),
                           (f.origin.code_iata.length() ? f.origin.code_iata : f.origin.code_icao).c_str(),
                           (f.destination.code_iata.length() ? f.destination.code_iata : f.destination.code_icao).c_str(),
                           (f.aircraft_display_name_short.length() ? f.aircraft_display_name_short : f.aircraft_code).c_str(),
                           f.baro_altitude, f.velocity, f.heading);
        }
        (void)enriched;
        } // else (ensureWiFi)
    }

    {
        static unsigned long lastHoldMs = 0;
        if (millis() - lastHoldMs >= 20000)
        {
            lastHoldMs = millis();
            char summary[96];
            int longest = 0;
            const int total = g_display.holdingSummary(summary, sizeof(summary), &longest);
            // Busy: eight or more holding, or someone holding for 15 minutes.
            static unsigned long lastBusyMs = 0;
            if ((total >= 8 || longest >= 15) && (!lastBusyMs || millis() - lastBusyMs > 3600000UL))
            {
                lastBusyMs = millis();
                Notify::post(Notify::Holding, 3, "hourglass_flowing_sand", "Busy holding stacks",
                             "%d holding at %s: %s", total, g_airport.name, summary);
            }
            g_webConfig.setHolding(summary);
            static int lastTotal = 0;
            if (total != lastTotal)
            {
                Log.printf("Holding: %d%s%s\n", total, total ? ": " : "", summary);
                lastTotal = total;
            }
        }
    }

    // The day's round-up at 22:30 local time, once a day.
    {
        static unsigned long lastCheckMs = 0;
        static int sentDay = -1;
        if (millis() - lastCheckMs >= 30000)
        {
            lastCheckMs = millis();
            const time_t t = time(nullptr);
            struct tm lt;
            if (t > 1600000000 && localtime_r(&t, &lt) && lt.tm_hour == 22 && lt.tm_min >= 30 && sentDay != lt.tm_yday)
            {
                sentDay = lt.tm_yday;
                char text[160];
                g_display.dailySummary(text, sizeof(text));
                Notify::post(Notify::Daily, 2, "bar_chart", "Today at the airport", "%s", text);
            }
        }
    }

    // One-tap update from GitHub (8.2), asked for on the web page: a
    // restart, then setup() downloads and installs it (utils/Ota.h).
    {
        char tag[16];
        if (Ota::takeRequest(tag, sizeof(tag)) && WiFi.status() == WL_CONNECTED)
        {
            saveStats();
            Ota::restartToInstall(tag);   // the download happens straight after, with a fresh heap
        }
    }

    // Phone notifications: one queued message per pass.
    if (WiFi.status() == WL_CONNECTED)
    {
        StageTrace::mark(StageTrace::Main, StageTrace::MainNotify);
        Notify::loop();
        HomeAssistant::loop(g_ha);   // MQTT: keeps the connection, publishes every 30 s
    }

    // Heathrow weather for the arrivals board and the night clock.
    // (8.1: no early request for the card's origin any more. Each one closed
    // and reopened the feed's TLS session, and the reopens fragmented the
    // heap; origins come with the regular request, or the page goes without.)
    if (WiFi.status() == WL_CONNECTED && millis() > 20000 && !g_display.showcaseRunning() &&
        (g_lastMetarMs == 0 || millis() - g_lastMetarMs >= g_metarWaitMs))
    {
        // The card's origin first, then the next arrivals'.
        if (g_cardOrigin[0] && !strstr(g_originIds, g_cardOrigin))
        {
            char ids[sizeof(g_originIds)];
            snprintf(ids, sizeof(ids), "%s%s%s", g_cardOrigin, g_originIds[0] ? "," : "", g_originIds);
            ids[sizeof(ids) - 1] = '\0';
            if (char *cut = strrchr(ids, ',')) if (strlen(ids) > 19) *cut = '\0';   // whole codes only
            strlcpy(g_originIds, ids, sizeof(g_originIds));
        }
        if (g_originIds[0]) Log.printf("Weather: also %s\n", g_originIds);
        g_lastMetarMs = millis();
        Metar m;
        bool gotWeather;
        {
            NetBusy busy;
            StageTrace::mark(StageTrace::Main, StageTrace::MainWeather);
            gotWeather = tlsAffordable("weather") &&
                         Weather::fetch(m, g_originIds, [](const Metar &o) { g_display.setOriginWeather(o); }, &g_taf);
        }
        heapCheckpoint("weather fetch");
        Health::noteWeather(gotWeather);
        if (gotWeather)
        {
            const bool changed = strcmp(m.raw, g_metar.raw) != 0;
            g_metar = m;
            char line[24];
            Weather::line(g_metar, line, sizeof(line));
            g_display.setWeather(g_metar);
            Weather::setQnh(g_metar.qnh);   // real heights near the airport (8.3)
            g_webConfig.setWeather(g_metar.raw);
            if (changed) Log.printf("Weather: %s -> \"%s\"\n", g_metar.raw, line);
            airfieldCheck();
            // The next report is due about 30 minutes after this one was
            // observed: ask a couple of minutes after that. The same report
            // again (not out yet): look again in five minutes.
            const time_t now = time(nullptr), obs = Weather::observedAt(g_metar, now);
            long waitS = 10 * 60;
            if (obs && !changed && now - obs > 25 * 60) waitS = 5 * 60;
            else if (obs)                               waitS = (long)(obs + 32 * 60 - now);
            waitS = waitS < 120 ? 120 : waitS > 1800 ? 1800 : waitS;
            g_metarWaitMs = (unsigned long)waitS * 1000UL;
        }
        else
            g_metarWaitMs = 60000;   // retry in a minute
    }

    // The display task draws the panel; keep the web preview's flight data
    // in step with whatever card it is showing.
    {
        const unsigned long webNow = millis();
        if (webNow - g_lastWebSyncMs >= 1000UL)
        {
            g_lastWebSyncMs = webNow;
            // The card can outlive the fetch list (e.g. after landing), so ask
            // the display what it is actually showing.
            static FlightInfo shown;
            const bool have = g_display.currentFlight(shown);
            g_webConfig.setCurrentFlight(have ? &shown : nullptr);
            // The card's origin, for its weather (8.0).
            g_cardOrigin[0] = '\0';
            if (have && shown.origin.code_icao.length() == 4 && shown.origin.code_icao != g_airport.icao)
                strlcpy(g_cardOrigin, shown.origin.code_icao.c_str(), sizeof(g_cardOrigin));
        }
    }

    // The logbook (8.1): sightings worth keeping as they happen, the day
    // after midnight; written here, outside the display lock.
    {
        static unsigned long lastLogbookMs = 0;
        if (millis() - lastLogbookMs > 15000)
        {
            lastLogbookMs = millis();
            LogoBook::saveMissingIfChanged();
            static bool yearDone = false;   // once the clock is set after a restart
            if (!yearDone && time(nullptr) > 1600000000) { yearDone = true; refreshYear(); }
            Logbook::Event ev[4];
            const int n = g_display.takeLogbookEvents(ev, 4);
            for (int i = 0; i < n; ++i) Logbook::addEvent(ev[i]);
            Logbook::Day day;
            std::vector<Logbook::Tally> tallies;
            if (g_display.takeLogbookDay(day, tallies))
            {
                char l1[20], l2[24];
                Logbook::addDay(day, tallies, l1, sizeof(l1), l2, sizeof(l2));
                g_display.setRecords(Logbook::records());
                if (l1[0]) g_display.showNews(l1, l2);
                refreshYear();
            }
        }
    }

    // Save today's stats every 5 minutes when they have changed (the file
    // write happens outside the display lock).
    {
        static unsigned long lastSaveMs = 0;
        if (millis() - lastSaveMs >= 5UL * 60 * 1000 && ESP.getFreeHeap() > 26000)
        {
            lastSaveMs = millis();
            saveStats();
        }
    }

    // Heartbeat: shows at a glance whether the display and web tasks are
    // still moving, and how the heap is holding up.
    {
        static unsigned long lastBeatMs = 0;
        static uint32_t lastFrames = 0, lastReqs = 0;
        const unsigned long beatNow = millis();
        if (beatNow - lastBeatMs >= 60000UL)
        {
            lastBeatMs = beatNow;
            const uint32_t frames = g_display.framesDrawn(), reqs = g_webConfig.requestsServed();
            // Stack left at its lowest (bytes) for the main loop, display and web tasks.
            // Kept under the log's 112-character lines (a longer one lost its tail).
            // fb: how many pieces the free heap is in (9.0.2): rising, it is
            // breaking up.
            multi_heap_info_t hi;
            heap_caps_get_info(&hi, MALLOC_CAP_8BIT);
            Log.printf("Beat %lus: +%u fr, +%u web, heap %u blk %u low %u, fb %u, stk %u/%u/%u\n",
                       beatNow / 1000, (unsigned)(frames - lastFrames), (unsigned)(reqs - lastReqs),
                       (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap(),
                       (unsigned)ESP.getMinFreeHeap(), (unsigned)hi.free_blocks,
                       (unsigned)uxTaskGetStackHighWaterMark(nullptr),
                       (unsigned)(g_display.task() ? uxTaskGetStackHighWaterMark(g_display.task()) : 0),
                       (unsigned)(g_webTask ? uxTaskGetStackHighWaterMark(g_webTask) : 0));
            lastFrames = frames;
            lastReqs   = reqs;
        }
    }
    if (millis() > 5UL * 60 * 1000) ImageCheck::keep("5 minutes up");
    Health::loop();
    StageTrace::mark(StageTrace::Main, StageTrace::MainIdle);
    delay(10);
}