/*
Purpose: Firmware entry point for ESP32.
Responsibilities:
- Initialize serial, connect to Wi‑Fi, and construct fetchers and display.
- Periodically fetch state vectors (OpenSky), enrich flights (AeroAPI), and render.
Configuration: UserConfiguration (location/filters/colors), TimingConfiguration (intervals),
               WiFiConfiguration (SSID/password), HardwareConfiguration (display specs).
*/
#include <vector>
#include <map>
#include <esp_task_wdt.h>
#include "utils/StageTrace.h"
#include <WiFi.h>
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
#include "adapters/LocalLogoStore.h"
#include "core/FlightDataFetcher.h"
#include "adapters/NeoMatrixDisplay.h"
#include "display/CardRenderer.h"
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
static LocalLogoStore             g_logoStore;
static FlightDataFetcher         *g_fetcher = nullptr;
static NeoMatrixDisplay g_display;

static unsigned long g_lastFetchMs = 0;
static unsigned long g_lastWebSyncMs = 0;
static std::vector<StateVector> g_states;
static std::vector<FlightInfo> g_flights;
static bool   g_wasNightSuppressed = false;  // tracks night-mode suppression for wake-up flush
static String g_wifiSsid;                   // active SSID  (NVS > compile-time)
static String g_wifiPass;                   // active password
static Metar  g_metar;                      // Heathrow weather, refreshed every few minutes
static unsigned long g_lastMetarMs = 0;
static constexpr unsigned long kMetarEveryMs = 10UL * 60 * 1000;

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
    }
    streak.swap(seen);
    for (auto it = alerted.begin(); it != alerted.end(); )
        it = now - it->second > 60UL * 60 * 1000 ? alerted.erase(it) : std::next(it);
}

// Map dot colour per airline (ICAO prefix of the call sign): the logo's accent,
// worked out once per airline. Unknown airlines and GA get a neutral white.
static Rgb airlineColour(const String &callsign)
{
    static std::map<String, Rgb> cache;
    const Rgb kNeutral{200, 205, 215};
    if (callsign.length() < 4 || !isalpha((unsigned char)callsign[0]) ||
        !isalpha((unsigned char)callsign[1]) || !isalpha((unsigned char)callsign[2]) ||
        !isdigit((unsigned char)callsign[3]))
        return kNeutral;
    String icao = callsign.substring(0, 3);
    icao.toUpperCase();
    auto it = cache.find(icao);
    if (it != cache.end()) return it->second;
    FlightInfo tmp;
    const Rgb c = g_logoStore.getAirlineLogo(icao, tmp.airline_logo_rgb565)
                      ? CardRenderer::accentFor(tmp) : kNeutral;
    if (cache.size() < 64) cache[icao] = c;
    return c;
}

// Every aircraft in range as a map point: position, track, speed, airline
// colour, and whether it is lined up on final for a Heathrow runway.
static std::vector<TrafficPoint> trafficFromStates(const std::vector<StateVector> &states)
{
    std::vector<TrafficPoint> pts;
    pts.reserve(states.size());
    for (const StateVector &s : states)
    {
        TrafficPoint p;
        p.id      = (uint32_t)strtoul(s.icao24.c_str(), nullptr, 16);
        p.lat     = s.lat;
        p.lon     = s.lon;
        p.heading = s.heading;
        p.gsKt    = isnan(s.velocity) ? NAN : s.velocity * 1.94384;
        p.colour  = squawkMeaning(s.squawk) ? Rgb{255, 40, 30} : airlineColour(s.callsign);

        FlightInfo f;   // just enough for the approach model
        f.lat = s.lat;  f.lon = s.lon;  f.heading = s.heading;
        f.baro_altitude = isnan(s.baro_altitude) ? NAN : s.baro_altitude * 3.28084;
        f.velocity      = p.gsKt;
        f.vertical_rate = isnan(s.vertical_rate) ? NAN : s.vertical_rate * 196.85;
        p.onFinal = ApproachModel::evaluate(f).phase == ApproachStatus::Approach;
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
    g_display.setArrivals(rows, n);
}

void setup()
{
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
    RareSpotter::begin();   // type log for "first sighting" (on the same LittleFS)
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
                nullptr,
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
    g_fetcher = new FlightDataFetcher(&g_stateFetcher, &g_flightFetcher, &g_logoStore);

    // Force the first fetch to fire on the very first loop() iteration rather
    // than waiting a full FETCH_INTERVAL_SECONDS from boot.
    // Unsigned wraparound is intentional — now - g_lastFetchMs will equal
    // exactly FETCH_INTERVAL_SECONDS * 1000 on the first call.
    g_lastFetchMs = millis() -
        (unsigned long)(g_config.fetch_interval_seconds * 1000UL);

    // From here the display task owns the panel (animation keeps running
    // while loop() blocks on network fetches).
    g_display.startTask();
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

    if (now - g_lastFetchMs >= intervalMs)
    {
        g_lastFetchMs = now;

        if (screenOffAtNight)
        {
            // Nothing to do — already logged the suppression above.
        }
        else if (!ensureWiFi())
        {
            Log.println("Skipping fetch — no WiFi");
        }
        else
        {
        size_t enriched;
        {
            NetBusy busy;
            enriched = g_fetcher->fetchFlights(g_states, g_flights);
        }

        // Dead-link watchdog: the ESP32 can stay "connected" to Wi-Fi with a
        // link that passes nothing (every DNS lookup fails). After a run of
        // fetches where no source answered, reconnect; if that doesn't help
        // either, restart.
        {
            static int failStreak = 0;
            // A fetch skipped for lack of heap (tlsAffordable) is not a dead link.
            const bool memoryShort = ESP.getFreeHeap() < 75000;
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
                delay(200);
                ESP.restart();
            }
        }
        g_display.displayFlights(g_flights);   // queues new contacts, refreshes telemetry
        g_display.updateTraffic(trafficFromStates(g_states));   // everything in range, for the map
        heapCheckpoint("fetch cycle");
        checkSquawks(g_states);
        g_display.noteTraffic(g_states);
        {
            char arr[12], dep[12];
            g_display.runwaysInUse(arr, sizeof(arr), dep, sizeof(dep));
            g_webConfig.setRunways(arr, dep);
        }
        updateArrivals(g_states, g_flights);

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

    // Heathrow weather for the arrivals board and the night clock.
    if (WiFi.status() == WL_CONNECTED && millis() > 20000 &&
        (g_lastMetarMs == 0 || millis() - g_lastMetarMs >= kMetarEveryMs))
    {
        g_lastMetarMs = millis();
        Metar m;
        bool gotWeather;
        {
            NetBusy busy;
            gotWeather = tlsAffordable("weather") && Weather::fetch(m);
        }
        heapCheckpoint("weather fetch");
        if (gotWeather)
        {
            const bool changed = strcmp(m.raw, g_metar.raw) != 0;
            g_metar = m;
            char line[24];
            Weather::line(g_metar, line, sizeof(line));
            g_display.setWeather(g_metar);
            g_webConfig.setWeather(g_metar.raw);
            if (changed) Log.printf("Weather: %s -> \"%s\"\n", g_metar.raw, line);
        }
        else
            g_lastMetarMs = millis() - kMetarEveryMs + 60000;   // retry in a minute
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
            g_webConfig.setCurrentFlight(g_display.currentFlight(shown) ? &shown : nullptr);
        }
    }

    // Save today's stats every 5 minutes when they have changed (the file
    // write happens outside the display lock).
    {
        static unsigned long lastSaveMs = 0;
        if (millis() - lastSaveMs >= 5UL * 60 * 1000 && ESP.getFreeHeap() > 40000)
        {
            lastSaveMs = millis();
            std::vector<uint8_t> data;
            if (g_display.statsSnapshot(data) && !DailyStats::writeFile(data))
                Log.println("Stats: could not save");
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
            Log.printf("Beat %lus: +%u frames, +%u web, heap %u blk %u low %u, %u routes\n",
                       beatNow / 1000, (unsigned)(frames - lastFrames), (unsigned)(reqs - lastReqs),
                       (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap(),
                       (unsigned)ESP.getMinFreeHeap(), (unsigned)(g_fetcher ? g_fetcher->cachedFlights() : 0));
            lastFrames = frames;
            lastReqs   = reqs;
        }
    }
    delay(10);
}