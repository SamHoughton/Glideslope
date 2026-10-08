/*
Purpose: WebConfig — minimal HTTP server, config API, and log streaming.
Uses WiFiServer/WiFiClient (already a project dependency) instead of the
WebServer library to avoid framework include-path issues.
*/
#include "utils/WebConfig.h"
#include "utils/TelnetLogger.h"
#include "utils/WifiProvisioner.h"
#include "utils/StageTrace.h"
#include "utils/BrandMark.h"
#include "config/RuntimeConfig.h"
#include "config/Airport.h"
#include <LittleFS.h>
#include "adapters/NeoMatrixDisplay.h"
#include <ArduinoJson.h>
#include <WiFi.h>
#include <Update.h>
#include <esp_task_wdt.h>
#include "utils/HeapWatch.h"
#include <vector>

WebConfig g_webConfig;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void WebConfig::begin(uint16_t port)
{
    if (!_displayMutex) _displayMutex = xSemaphoreCreateMutex();
    _server = WiFiServer(port);
    _server.begin();
    Log.printf("WebConfig: UI at http://%s/\n", WiFi.localIP().toString().c_str());
}

void WebConfig::loop()
{
    StageTrace::mark(StageTrace::Web, StageTrace::WebAccept);
    // While the main loop is in a TLS handshake the heap briefly drops by
    // 60-70 KB; serving a request then (its buffers, the reply) could leave
    // the handshake short. New requests wait in the backlog until it passes.
    if (g_netBusy || ESP.getFreeHeap() < kMinHeapToServe) { delay(20); return; }
    WiFiClient client = _server.accept();
    if (!client) { StageTrace::mark(StageTrace::Web, StageTrace::WebIdle); return; }
    ++_requests;
    // Accepted sockets block with no limit by default: a browser that stops
    // reading (background tab, phone asleep) would hang the web task. Bound
    // every send/receive on this connection to 3 s.
    client.setTimeout(3);
    StageTrace::mark(StageTrace::Web, StageTrace::WebReadRequest);

    // ── Read request headers (until CRLFCRLF) ─────────────────────────────
    String raw;
    raw.reserve(512);
    bool headersRead = false;
    unsigned long t0 = millis();
    while (client.connected() && millis() - t0 < 2000 && !headersRead)
    {
        while (client.available() && !headersRead)
        {
            raw += (char)client.read();
            if (raw.endsWith("\r\n\r\n"))
                headersRead = true;
        }
        if (!headersRead) delay(1);
    }
    if (!headersRead) { client.stop(); return; }

    // ── Parse request line ─────────────────────────────────────────────────
    int sp1 = raw.indexOf(' ');
    int sp2 = raw.indexOf(' ', sp1 + 1);
    if (sp1 < 0 || sp2 < 0) { client.stop(); return; }

    Req r;
    r.method = raw.substring(0, sp1);
    String fullPath = raw.substring(sp1 + 1, sp2);
    int qpos = fullPath.indexOf('?');
    if (qpos >= 0)
    {
        r.path  = fullPath.substring(0, qpos);
        r.query = fullPath.substring(qpos + 1);
    }
    else
    {
        r.path = fullPath;
    }

    // ── Read POST body (Content-Length driven) ─────────────────────────────
    if (r.method == "POST")
    {
        String lower = raw;
        lower.toLowerCase();
        int clIdx = lower.indexOf("content-length: ");
        if (clIdx >= 0)
        {
            int clEnd = raw.indexOf("\r\n", clIdx + 16);
            int bodyLen = raw.substring(clIdx + 16, clEnd).toInt();
            r.contentLength = bodyLen;
            // A firmware image is streamed straight to flash by its handler.
            if (r.path == "/api/update" || r.path == "/api/airport") bodyLen = 0;
            if (bodyLen > 0 && bodyLen < 8192)
            {
                r.body.reserve(bodyLen + 1);
                unsigned long bt = millis();
                while ((int)r.body.length() < bodyLen && millis() - bt < 3000)
                {
                    if (client.available()) r.body += (char)client.read();
                    else delay(1);
                }
            }
        }
    }

    // ── Route ──────────────────────────────────────────────────────────────
    // Path length identifies the route in hang reports (1 = page, 10 = frame,
    // 11 = config, 12 = display, 8 = log, 14+ = demo).
    StageTrace::mark(StageTrace::Web, StageTrace::WebHandle, (uint8_t)r.path.length());
    if      (r.path == "/"                  && r.method == "GET")  handleRoot(client);
    else if (r.path == "/api/config"        && r.method == "GET")  handleGetConfig(client);
    else if (r.path == "/api/config"        && r.method == "POST") handlePostConfig(client, r);
    else if (r.path == "/api/config/reset"  && r.method == "POST") handleResetConfig(client);
    else if (r.path == "/api/restart"       && r.method == "POST") handleRestart(client);
    else if (r.path == "/api/wifi/reset"    && r.method == "POST") handleWifiReset(client);
    else if (r.path == "/api/log"           && r.method == "GET")  handleGetLog(client, r);
    else if (r.path == "/api/display"       && r.method == "GET")  handleGetDisplay(client);
    else if (r.path == "/api/frame"         && r.method == "GET")  handleGetFrame(client);
    else if (r.path == "/api/status"        && r.method == "GET")  handleGetStatus(client);
    else if (r.path == "/api/update"        && r.method == "POST") handleUpdate(client, r.contentLength);
    else if (r.path == "/api/airport"       && r.method == "POST") handleAirport(client, r.contentLength);
    else if (r.path == "/api/airport/reset" && r.method == "POST") handleAirportReset(client);
    else if (r.path == "/api/demo/showcase" && r.method == "POST") { requestShowcase(); sendHttp(client, 200, "application/json", "{\"ok\":true}"); }
    else if (r.path == "/api/demo/takeoff"  && r.method == "POST") { requestTakeoffDemo(); sendHttp(client, 200, "application/json", "{\"ok\":true}"); }
    else if (r.path == "/api/demo/squawk"   && r.method == "POST") { requestAlertDemo(); sendHttp(client, 200, "application/json", "{\"ok\":true}"); }
    else if (r.path == "/api/demo/weather"  && r.method == "POST") { requestScreenPreview(4, 10000); sendHttp(client, 200, "application/json", "{\"ok\":true}"); }
    else if (r.path == "/api/demo/arrivals" && r.method == "POST") { requestScreenPreview(3, 10000); sendHttp(client, 200, "application/json", "{\"ok\":true}"); }
    else if (r.path == "/api/demo/sprites"  && r.method == "POST") { requestSpriteGallery(10000); sendHttp(client, 200, "application/json", "{\"ok\":true}"); }
    else if (r.path == "/api/demo/splash"   && r.method == "POST") { requestSplashPreview(10000); sendHttp(client, 200, "application/json", "{\"ok\":true}"); }
    else if (r.path == "/api/demo/map"      && r.method == "POST") { requestMapPreview(30000); sendHttp(client, 200, "application/json", "{\"ok\":true}"); }
    else if (r.path == "/api/demo/stats"    && r.method == "POST") { requestScreenPreview(1, 10000); sendHttp(client, 200, "application/json", "{\"ok\":true}"); }
    else if (r.path == "/api/demo/clock"    && r.method == "POST") { requestScreenPreview(2, 10000); sendHttp(client, 200, "application/json", "{\"ok\":true}"); }
    else if (r.path == "/api/demo/landing"  && r.method == "POST") { requestLandingReplay(); sendHttp(client, 200, "application/json", "{\"ok\":true}"); }
    else if (r.path == "/api/demo/rare"     && r.method == "POST") { requestRareSpotDemo(); sendHttp(client, 200, "application/json", "{\"ok\":true}"); }
    else if (r.path == "/api/demo/goaround" && r.method == "POST") { requestGoAroundDemo(); sendHttp(client, 200, "application/json", "{\"ok\":true}"); }
    else if (r.path == "/api/demo/flyacross" && r.method == "POST")
    {
        const String dir = qparam(r.query, "dir");
        requestFlyAcrossReplay(dir == "right" ? 1 : dir == "left" ? -1 : 0);
        sendHttp(client, 200, "application/json", "{\"ok\":true}");
    }
    else sendHttp(client, 404, "text/plain", "Not found");

    client.flush();
    client.stop();
    heapCheckpoint("web request");
    StageTrace::mark(StageTrace::Web, StageTrace::WebIdle);
}

// ---------------------------------------------------------------------------
// HTTP helpers
// ---------------------------------------------------------------------------

void WebConfig::sendHttp(WiFiClient &c, int code,
                          const char *contentType, const String &body)
{
    const char *reason = (code == 200) ? "OK"
                       : (code == 400) ? "Bad Request"
                       : (code == 500) ? "Internal Server Error"
                       :                 "Not Found";
    c.printf("HTTP/1.1 %d %s\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %u\r\n"
             "Connection: close\r\n\r\n",
             code, reason, contentType, (unsigned)body.length());
    sendChunked(c, (const uint8_t *)body.c_str(), body.length(), 1024);
}

void WebConfig::sendHtmlDirect(WiFiClient &c, const char *html, size_t len)
{
    c.printf("HTTP/1.1 200 OK\r\n"
             "Content-Type: text/html; charset=utf-8\r\n"
             "Content-Length: %u\r\n"
             "Connection: close\r\n\r\n",
             (unsigned)len);
    sendChunked(c, (const uint8_t *)html, len, 512);
}

// Writes in chunks and gives up as soon as the browser has gone: a client that
// disappears mid-transfer would otherwise stall the web task for each chunk.
void WebConfig::sendChunked(WiFiClient &c, const uint8_t *data, size_t len, size_t chunk)
{
    for (size_t sent = 0; sent < len; )
    {
        if (!c.connected()) return;
        const size_t n = c.write(data + sent, min(chunk, len - sent));
        if (n == 0) return;
        sent += n;
    }
}

String WebConfig::qparam(const String &query, const char *key)
{
    String k = String(key) + "=";
    int pos = query.indexOf(k);
    if (pos < 0) return "";
    int start = pos + k.length();
    int end = query.indexOf('&', start);
    return end < 0 ? query.substring(start) : query.substring(start, end);
}

// ---------------------------------------------------------------------------
// Route handlers
// ---------------------------------------------------------------------------

// Declared at the bottom of this file as a raw string literal
extern const char kHtmlPage[];
extern const size_t kHtmlPageLen;

void WebConfig::handleRoot(WiFiClient &c)
{
    sendHtmlDirect(c, kHtmlPage, kHtmlPageLen);
}

void WebConfig::handleGetConfig(WiFiClient &c)
{
    JsonDocument doc;
    doc["tar1090_host"]                  = String(g_config.tar1090_host);
    doc["center_lat"]                    = g_config.center_lat;
    doc["center_lon"]                    = g_config.center_lon;
    doc["home_lat"]                      = g_config.home_lat;
    doc["home_lon"]                      = g_config.home_lon;
    doc["radius_km"]                     = g_config.radius_km;
    doc["min_altitude_ft"]               = g_config.min_altitude_ft;
    doc["display_brightness"]            = g_config.display_brightness;
    doc["text_color_r"]                  = g_config.text_color_r;
    doc["text_color_g"]                  = g_config.text_color_g;
    doc["text_color_b"]                  = g_config.text_color_b;
    doc["display_nearest_only"]          = g_config.display_nearest_only;
    doc["display_border"]                = g_config.display_border;
    doc["show_flight_number_with_logo"]  = g_config.show_flight_number_with_logo;
    doc["show_aircraft_type"]            = g_config.show_aircraft_type;
    doc["show_aircraft_registration"]    = g_config.show_aircraft_registration;
    doc["swap_aircraft_type_reg"]        = g_config.swap_aircraft_type_reg;
    doc["screen_facing"]                 = String(g_config.screen_facing);
    doc["display_flip"]                  = g_config.display_flip;
    doc["night_mode_enabled"]            = g_config.night_mode_enabled;
    doc["night_start_minutes"]           = g_config.night_start_minutes;
    doc["night_end_minutes"]             = g_config.night_end_minutes;
    doc["night_brightness"]              = g_config.night_brightness;
    doc["utc_offset_minutes"]            = g_config.utc_offset_minutes;
    doc["fetch_interval_seconds"]        = g_config.fetch_interval_seconds;
    doc["local_fetch_interval_seconds"]  = g_config.local_fetch_interval_seconds;
    doc["display_cycle_seconds"]         = g_config.display_cycle_seconds;
    doc["card_lead_seconds"]             = g_config.card_lead_seconds;
    doc["interlude_seconds"]             = g_config.interlude_seconds;
    doc["screens"]                       = g_config.screens;
    doc["aeroapi_cache_ttl_seconds"]     = g_config.aeroapi_cache_ttl_seconds;
    doc["aeroapi_fail_cache_ttl_seconds"] = g_config.aeroapi_fail_cache_ttl_seconds;

    // API keys: client ID is not secret; secrets are masked so they are never
    // transmitted back to the browser, but a "***" sentinel signals they are set.
    doc["opensky_client_id"]     = String(g_config.opensky_client_id);
    doc["opensky_client_secret"] = (strlen(g_config.opensky_client_secret) > 0) ? "***" : "";
    doc["opensky_priority"]      = g_config.opensky_priority;
    doc["use_community_feeds"]   = g_config.use_community_feeds;
    doc["aeroapi_key"]           = (strlen(g_config.aeroapi_key)           > 0) ? "***" : "";

    String out;
    serializeJson(doc, out);
    sendHttp(c, 200, "application/json", out);
}

void WebConfig::handlePostConfig(WiFiClient &c, const Req &r)
{
    if (r.body.isEmpty())
    {
        sendHttp(c, 400, "application/json", "{\"ok\":false,\"error\":\"empty body\"}");
        return;
    }

    JsonDocument doc;
    if (deserializeJson(doc, r.body))
    {
        sendHttp(c, 400, "application/json", "{\"ok\":false,\"error\":\"JSON parse error\"}");
        return;
    }

    // ArduinoJson "| current" idiom: keeps existing value when key is absent
    {
        const char *v = doc["tar1090_host"] | g_config.tar1090_host;
        strncpy(g_config.tar1090_host, v, sizeof(g_config.tar1090_host) - 1);
        g_config.tar1090_host[sizeof(g_config.tar1090_host) - 1] = '\0';
    }
    g_config.center_lat   = doc["center_lat"]   | g_config.center_lat;
    g_config.center_lon   = doc["center_lon"]   | g_config.center_lon;
    g_config.home_lat     = doc["home_lat"]     | g_config.home_lat;
    g_config.home_lon     = doc["home_lon"]     | g_config.home_lon;
    g_config.radius_km    = doc["radius_km"]    | g_config.radius_km;
    g_config.min_altitude_ft = doc["min_altitude_ft"] | g_config.min_altitude_ft;

    g_config.display_brightness = (uint8_t)(doc["display_brightness"] | (int)g_config.display_brightness);
    g_config.text_color_r       = (uint8_t)(doc["text_color_r"]       | (int)g_config.text_color_r);
    g_config.text_color_g       = (uint8_t)(doc["text_color_g"]       | (int)g_config.text_color_g);
    g_config.text_color_b       = (uint8_t)(doc["text_color_b"]       | (int)g_config.text_color_b);

    g_config.display_nearest_only         = doc["display_nearest_only"]         | g_config.display_nearest_only;
    g_config.display_border               = doc["display_border"]               | g_config.display_border;
    g_config.show_flight_number_with_logo = doc["show_flight_number_with_logo"] | g_config.show_flight_number_with_logo;
    g_config.show_aircraft_type           = doc["show_aircraft_type"]           | g_config.show_aircraft_type;
    g_config.show_aircraft_registration   = doc["show_aircraft_registration"]   | g_config.show_aircraft_registration;
    g_config.swap_aircraft_type_reg       = doc["swap_aircraft_type_reg"]       | g_config.swap_aircraft_type_reg;

    const char *facing = doc["screen_facing"] | g_config.screen_facing;
    strncpy(g_config.screen_facing, facing, sizeof(g_config.screen_facing) - 1);
    g_config.screen_facing[sizeof(g_config.screen_facing) - 1] = '\0';

    g_config.display_flip          = doc["display_flip"]          | g_config.display_flip;
    g_config.night_mode_enabled    = doc["night_mode_enabled"]    | g_config.night_mode_enabled;
    g_config.night_start_minutes   = (uint16_t)(doc["night_start_minutes"] | (int)g_config.night_start_minutes);
    g_config.night_end_minutes     = (uint16_t)(doc["night_end_minutes"]   | (int)g_config.night_end_minutes);
    g_config.night_brightness      = (uint8_t)(doc["night_brightness"]     | (int)g_config.night_brightness);
    g_config.utc_offset_minutes    = doc["utc_offset_minutes"]    | g_config.utc_offset_minutes;

    g_config.opensky_priority              = doc["opensky_priority"]              | g_config.opensky_priority;
    g_config.use_community_feeds           = doc["use_community_feeds"]           | g_config.use_community_feeds;

    g_config.fetch_interval_seconds        = doc["fetch_interval_seconds"]        | g_config.fetch_interval_seconds;
    g_config.local_fetch_interval_seconds  = doc["local_fetch_interval_seconds"]  | g_config.local_fetch_interval_seconds;
    g_config.display_cycle_seconds         = doc["display_cycle_seconds"]         | g_config.display_cycle_seconds;
    g_config.card_lead_seconds             = doc["card_lead_seconds"]             | g_config.card_lead_seconds;
    g_config.interlude_seconds             = doc["interlude_seconds"]             | g_config.interlude_seconds;
    g_config.screens                       = (uint8_t)(doc["screens"]             | (int)g_config.screens);
    g_config.aeroapi_cache_ttl_seconds     = doc["aeroapi_cache_ttl_seconds"]      | g_config.aeroapi_cache_ttl_seconds;
    g_config.aeroapi_fail_cache_ttl_seconds = doc["aeroapi_fail_cache_ttl_seconds"] | g_config.aeroapi_fail_cache_ttl_seconds;

    // API keys — only update when the posted value is non-empty and not the
    // mask sentinel "***".  Empty / absent = keep the current value intact.
    {
        const char *v = doc["opensky_client_id"] | "";
        if (strlen(v) > 0 && strcmp(v, "***") != 0) {
            strncpy(g_config.opensky_client_id, v, sizeof(g_config.opensky_client_id) - 1);
            g_config.opensky_client_id[sizeof(g_config.opensky_client_id) - 1] = '\0';
        }
    }
    {
        const char *v = doc["opensky_client_secret"] | "";
        if (strlen(v) > 0 && strcmp(v, "***") != 0) {
            strncpy(g_config.opensky_client_secret, v, sizeof(g_config.opensky_client_secret) - 1);
            g_config.opensky_client_secret[sizeof(g_config.opensky_client_secret) - 1] = '\0';
        }
    }
    {
        const char *v = doc["aeroapi_key"] | "";
        if (strlen(v) > 0 && strcmp(v, "***") != 0) {
            strncpy(g_config.aeroapi_key, v, sizeof(g_config.aeroapi_key) - 1);
            g_config.aeroapi_key[sizeof(g_config.aeroapi_key) - 1] = '\0';
        }
    }

    saveConfig();
    Log.println("WebConfig: settings updated and saved to NVS");
    sendHttp(c, 200, "application/json", "{\"ok\":true}");
}

void WebConfig::handleResetConfig(WiFiClient &c)
{
    resetConfig();
    Log.println("WebConfig: settings reset to firmware defaults");
    sendHttp(c, 200, "application/json", "{\"ok\":true}");
}

void WebConfig::handleRestart(WiFiClient &c)
{
    Log.println("WebConfig: restart requested via web UI");
    sendHttp(c, 200, "application/json", "{\"ok\":true}");
    c.flush();
    c.stop();
    delay(200);
    ESP.restart();
}

void WebConfig::handleWifiReset(WiFiClient &c)
{
    Log.println("WebConfig: WiFi credentials cleared via web UI — restarting into setup AP");
    sendHttp(c, 200, "application/json", "{\"ok\":true}");
    c.flush();
    c.stop();
    delay(200);
    WifiProvisioner::clearCredentials();
    ESP.restart();
}

// Runs on the main loop. Builds the "now showing" JSON here and swaps it in
// under a lock, so the web task never reads a FlightInfo that is being rewritten.
void WebConfig::setCurrentFlight(const FlightInfo *f)
{
    JsonDocument doc;
    doc["active"] = (f != nullptr);
    if (f)
    {
        const String ident = f->ident.length() ? f->ident : f->ident_icao;
        doc["ident"]         = ident;
        doc["flight"]        = f->ident_iata.length() ? f->ident_iata : ident;
        doc["airline_name"]  = f->airline_display_name_full.length() ? f->airline_display_name_full
                             : f->operator_icao.length()             ? f->operator_icao
                             : f->operator_code;
        doc["origin"]        = f->origin.code_iata.length()      ? f->origin.code_iata      : f->origin.code_icao;
        doc["dest"]          = f->destination.code_iata.length() ? f->destination.code_iata : f->destination.code_icao;
        doc["aircraft_name"] = f->aircraft_display_name_short.length()
                                 ? f->aircraft_display_name_short : f->aircraft_code;
        doc["registration"]  = f->registration;
        doc["altitude_ft"]   = isnan(f->baro_altitude) ? 0 : (int)round(f->baro_altitude);
        doc["speed_kt"]      = isnan(f->velocity)      ? 0 : (int)round(f->velocity);
        doc["heading_deg"]   = isnan(f->heading)       ? 0 : (int)round(f->heading);
        doc["vspeed_fpm"]    = isnan(f->vertical_rate) ? 0 : (int)round(f->vertical_rate);
    }
    String json;
    serializeJson(doc, json);

    if (_displayMutex) xSemaphoreTake(_displayMutex, portMAX_DELAY);
    _displayJson = json;
    if (_displayMutex) xSemaphoreGive(_displayMutex);
}

void WebConfig::handleGetDisplay(WiFiClient &c)
{
    if (_displayMutex) xSemaphoreTake(_displayMutex, portMAX_DELAY);
    const String out = _displayJson;
    if (_displayMutex) xSemaphoreGive(_displayMutex);
    sendHttp(c, 200, "application/json", out);
}

// Health at a glance: uptime, why it last reset, memory, task activity.
void WebConfig::handleGetStatus(WiFiClient &c)
{
    JsonDocument doc;
    doc["version"]        = GLIDESLOPE_VERSION;
    doc["built"]          = __DATE__ " " __TIME__;
    doc["uptime_s"]       = millis() / 1000;
    doc["last_reset"]     = StageTrace::lastReset();
    doc["heap_free"]      = ESP.getFreeHeap();
    doc["heap_max_block"] = ESP.getMaxAllocHeap();
    doc["heap_min_free"]  = ESP.getMinFreeHeap();
    doc["display_frames"] = displayFramesDrawn();
    doc["web_requests"]   = (uint32_t)_requests;
    if (_displayMutex) xSemaphoreTake(_displayMutex, portMAX_DELAY);
    doc["metar"]          = _metar;
    doc["airport"]        = g_airport.icao;
    doc["airport_name"]   = g_airport.name;
    doc["airport_lat"]    = g_airport.lat;
    doc["airport_lon"]    = g_airport.lon;
    doc["airport_pack"]   = !g_airport.builtIn;
    if (_displayMutex) xSemaphoreGive(_displayMutex);
    String out;
    serializeJson(doc, out);
    sendHttp(c, 200, "application/json", out);
}

void WebConfig::setWeather(const char *metar)
{
    if (_displayMutex) xSemaphoreTake(_displayMutex, portMAX_DELAY);
    _metar = metar;
    if (_displayMutex) xSemaphoreGive(_displayMutex);
}

// Over-the-air update: the request body is a firmware .bin (the same file
// PlatformIO builds, or firmware.bin from a GitHub release). It is written to
// the spare app slot and the board restarts into it; anything that fails
// leaves the running firmware untouched.
void WebConfig::handleUpdate(WiFiClient &c, int length)
{
    auto fail = [&](const char *why) {
        Log.printf("Update: failed: %s\n", why);
        // Read (and discard) the rest of the upload first: closing with unread
        // data resets the connection and the browser never sees the reason.
        uint8_t sink[512];
        const unsigned long t0 = millis();
        while (c.connected() && millis() - t0 < 20000)
        {
            esp_task_wdt_reset();
            if (c.available()) { c.read(sink, sizeof(sink)); continue; }
            delay(5);
            if (!c.available()) { delay(50); if (!c.available()) break; }
        }
        String body = String("{\"ok\":false,\"error\":\"") + why + "\"}";
        sendHttp(c, 400, "application/json", body);
    };
    if (length <= 0) { fail("empty upload"); return; }
    if ((uint32_t)length > ESP.getFreeSketchSpace()) { fail("file too large for the app slot"); return; }
    if (!Update.begin(length, U_FLASH)) { fail(Update.errorString()); return; }
    Log.printf("Update: receiving %d bytes\n", length);
    requestPanelMessage("UPDATING");

    uint8_t buf[1024];
    int got = 0, lastTenth = -1;
    unsigned long lastData = millis();
    while (got < length)
    {
        esp_task_wdt_reset();
        const int avail = c.available();
        // The first read must hold the image header (checked below).
        if (avail <= 0 || (got == 0 && avail < 36 && length >= 36))
        {
            if (!c.connected() || millis() - lastData > 10000) break;
            delay(2);
            continue;
        }
        const int n = c.read(buf, min((int)sizeof(buf), min(avail, length - got)));
        if (n <= 0) continue;
        if (got == 0)
        {
            // ESP32 image: magic 0xE9, chip id at byte 12 (9 = ESP32-S3), and an
            // application (not a bootloader or merged image): the app
            // description's magic word 0xABCD5432 at byte 32.
            const uint16_t chip = buf[12] | (buf[13] << 8);
            const uint32_t appMagic = buf[32] | (buf[33] << 8) | (buf[34] << 16) | ((uint32_t)buf[35] << 24);
            const char *why = n < 36 || buf[0] != 0xE9 ? "not a firmware image"
                            : chip != 9                ? "firmware is for a different chip"
                            : appMagic != 0xABCD5432   ? "not an app image (use firmware.bin, not the full image)"
                            : nullptr;
            if (why)
            {
                Update.abort();
                fail(why);
                return;
            }
        }
        if (Update.write(buf, n) != (size_t)n)
        {
            Update.abort();
            fail(Update.errorString());
            return;
        }
        got += n;
        lastData = millis();
        const int tenth = (int)((int64_t)got * 10 / length);
        if (tenth != lastTenth)
        {
            lastTenth = tenth;
            char msg[16];
            snprintf(msg, sizeof(msg), "UPDATING %d%%", tenth * 10);
            requestPanelMessage(msg);
        }
    }
    if (got != length) { Update.abort(); fail("upload interrupted"); return; }
    if (!Update.end(true)) { fail(Update.errorString()); return; }

    Log.println("Update: installed, restarting");
    requestPanelMessage("RESTARTING");
    sendHttp(c, 200, "application/json", "{\"ok\":true}");
    c.flush();
    c.stop();
    delay(500);
    ESP.restart();
}

// Airport pack: streamed to a temporary file, checked (header parses, map is
// the right size), then swapped in; the board restarts to load it.
void WebConfig::handleAirport(WiFiClient &c, int length)
{
    auto reply = [&](bool ok, const String &msg) {
        String body = ok ? String("{\"ok\":true,\"airport\":\"") + msg + "\"}"
                         : String("{\"ok\":false,\"error\":\"") + msg + "\"}";
        sendHttp(c, ok ? 200 : 400, "application/json", body);
    };
    if (length <= (int)AirportPack::kMapBytes || length > 8192)
    {
        // Read the upload first: replying with unread data resets the
        // connection and the browser never sees the reason.
        uint8_t sink[256];
        int left = length;
        const unsigned long t0 = millis();
        while (left > 0 && c.connected() && millis() - t0 < 10000)
        {
            const int n = c.read(sink, min((int)sizeof(sink), left));
            if (n > 0) left -= n; else delay(2);
        }
        reply(false, "not an airport pack (wrong size)");
        return;
    }

    const char *tmp = "/airport.tmp";
    File f = LittleFS.open(tmp, "w");
    if (!f) { reply(false, "cannot write to the file system"); return; }
    uint8_t buf[512];
    int got = 0;
    unsigned long lastData = millis();
    while (got < length && c.connected() && millis() - lastData < 10000)
    {
        const int n = c.read(buf, min((int)sizeof(buf), length - got));
        if (n <= 0) { delay(2); continue; }
        f.write(buf, n);
        got += n;
        lastData = millis();
    }
    f.close();
    if (got != length) { LittleFS.remove(tmp); reply(false, "upload interrupted"); return; }

    // Check it before replacing the current pack.
    f = LittleFS.open(tmp, "r");
    const String header = f.readStringUntil((char)10);   // newline ends the header
    const size_t rest = f.size() - f.position();
    f.close();
    Airport a;
    String error;
    if (!AirportPack::parseHeader(header.c_str(), a, error) || rest != AirportPack::kMapBytes)
    {
        LittleFS.remove(tmp);
        reply(false, error.length() ? error : String("map is the wrong size"));
        return;
    }
    LittleFS.remove(AirportPack::kPath);
    LittleFS.rename(tmp, AirportPack::kPath);
    Log.printf("WebConfig: airport pack %s (%s) installed, restarting\n", a.name, a.icao);
    reply(true, a.icao);
    c.flush();
    c.stop();
    delay(500);
    ESP.restart();
}

void WebConfig::handleAirportReset(WiFiClient &c)
{
    LittleFS.remove(AirportPack::kPath);
    Log.println("WebConfig: airport pack removed, back to Heathrow; restarting");
    sendHttp(c, 200, "application/json", "{\"ok\":true}");
    c.flush();
    c.stop();
    delay(500);
    ESP.restart();
}

// Raw 128x64 RGB565 frame currently on the panel (little-endian, row-major).
void WebConfig::handleGetFrame(WiFiClient &c)
{
    const size_t len = FrameCanvas::W * FrameCanvas::H * sizeof(uint16_t);
    c.printf("HTTP/1.1 200 OK\r\n"
             "Content-Type: application/octet-stream\r\n"
             "Cache-Control: no-store\r\n"
             "Content-Length: %u\r\n"
             "Connection: close\r\n\r\n",
             (unsigned)len);
    sendChunked(c, (const uint8_t *)g_shownFrame.pixels(), len, 1024);
}

void WebConfig::handleGetLog(WiFiClient &c, const Req &r)
{
    uint32_t cursor = 0;
    String cv = qparam(r.query, "cursor");
    if (cv.length()) cursor = (uint32_t)cv.toInt();

    // Built in a static buffer (only the web task calls this): a log reply
    // every 1.5 s used to allocate and free several KB, fragmenting the heap.
    // Sub-steps for hang reports: 82 building, 83 sending.
    static char s_json[4 * 1024];
    StageTrace::mark(StageTrace::Web, StageTrace::WebHandle, 82);
    size_t n = Log.linesJson(cursor, 32, s_json, sizeof(s_json));
    if (n == 0) n = snprintf(s_json, sizeof(s_json), "{\"cursor\":%lu,\"lines\":[]}", (unsigned long)cursor);
    StageTrace::mark(StageTrace::Web, StageTrace::WebHandle, 83);
    c.printf("HTTP/1.1 200 OK\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %u\r\n"
             "Connection: close\r\n\r\n",
             (unsigned)n);
    sendChunked(c, (const uint8_t *)s_json, n, 1024);
}

// ---------------------------------------------------------------------------
// Embedded single-page UI
// ---------------------------------------------------------------------------

// Attribute values and JS strings use single quotes so the C string needs no escaping.
const char kHtmlPage[] =
"<!DOCTYPE html>"
"<html lang='en'>"
"<head>"
"<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Glideslope</title>"
"<link rel='icon' type='image/svg+xml' href=\"" GS_FAVICON_URI "\">"
"<meta name='theme-color' content='#0d1117'>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{background:#0d1117;color:#c9d1d9;font:14px/1.5 system-ui,-apple-system,'Segoe UI',sans-serif}"
"header{padding:12px 18px;background:#161b22;border-bottom:1px solid #30363d;display:flex;align-items:center;gap:12px}"
"header svg{width:28px;height:28px;flex:none}header b{letter-spacing:.08em}header b i{font-style:normal;color:#ffb93c}"
"header b{font-size:16px;color:#f0f6fc}header span{color:#8b949e;font-size:13px}"
".wrap{display:grid;grid-template-columns:minmax(0,560px) minmax(0,1fr);gap:18px;padding:18px;max-width:1200px}"
"@media(max-width:900px){.wrap{grid-template-columns:minmax(0,1fr);padding:12px;gap:12px}}"
".card{background:#161b22;border:1px solid #30363d;border-radius:8px;padding:14px 16px;margin-bottom:16px}"
"h2{color:#8b949e;font-size:12px;text-transform:uppercase;letter-spacing:.06em;margin-bottom:10px}"
"#dmp{display:block;width:100%;height:auto;background:#050608;border-radius:4px;image-rendering:pixelated}"
"#now{margin-top:8px;color:#8b949e;font-size:13px;min-height:20px}#now b{color:#f0f6fc}"
".row{display:grid;grid-template-columns:1fr 1fr;gap:10px}"
".f{margin-bottom:10px}"
"label{display:block;color:#8b949e;font-size:12px;margin-bottom:4px}"
"small{color:#6e7681;font-size:11px}"
"input,select{width:100%;background:#0d1117;border:1px solid #30363d;color:#c9d1d9;padding:6px 8px;border-radius:5px;font:inherit}"
"input:focus,select:focus{outline:2px solid #388bfd;border-color:#388bfd}"
"input[type=range]{padding:0;accent-color:#388bfd}"
".ck{display:flex;gap:8px;align-items:flex-start;margin-bottom:8px;color:#c9d1d9;font-size:13px;cursor:pointer}"
".ck input{width:auto;margin-top:3px;accent-color:#238636}"
".btns{display:flex;flex-wrap:wrap;gap:8px}"
"button{border:1px solid #30363d;background:#21262d;color:#c9d1d9;padding:8px 14px;border-radius:6px;cursor:pointer;font:inherit;font-size:13px;min-height:36px}"
"button:hover{border-color:#8b949e}"
"button.go{background:#238636;border-color:#2ea043;color:#fff}"
"details summary{cursor:pointer;color:#8b949e;font-size:12px;text-transform:uppercase;letter-spacing:.06em}"
"details[open] summary{margin-bottom:10px}"
".bar{position:sticky;bottom:0;background:#0d1117;padding:10px 0;display:flex;gap:10px;align-items:center;flex-wrap:wrap}"
"#st{color:#8b949e;font-size:13px}"
"#lb{height:260px;background:#0d1117;border:1px solid #21262d;border-radius:4px;overflow-y:auto;padding:8px;font:11px/1.4 ui-monospace,Consolas,monospace;white-space:pre-wrap;word-break:break-all}"
"input[type=time]::-webkit-calendar-picker-indicator{filter:invert(.7)}"
"</style>"
"</head>"
"<body>"
"<header>" GS_BRAND_SVG "<b>GLIDE<i>SLOPE</i></b><span id='hdr'></span></header>"
"<div class='wrap'>"

// ── Left: live panel, animations, log ───────────────────────────────────────
"<div>"
"<div class='card'>"
"<h2>On the panel now</h2>"
"<canvas id='dmp' width='512' height='256'></canvas>"
"<div id='now'>&nbsp;</div>"
"</div>"
"<div class='card'>"
"<h2>Animations</h2>"
"<div class='btns'>"
"<button data-demo='showcase' class='go'>Showcase (35 s)</button>"
"<button data-demo='flyacross'>Replay fly-across</button>"
"<button data-demo='landing'>Replay landing</button>"
"<button data-demo='rare'>Rare spot</button>"
"<button data-demo='goaround'>Go-around</button>"
"<button data-demo='takeoff'>Take-off</button>"
"<button data-demo='squawk'>Emergency squawk</button>"
"<button data-demo='arrivals'>Arrivals board (10 s)</button>"
"<button data-demo='weather'>Weather (10 s)</button>"
"<button data-demo='sprites'>Aircraft sprites (10 s)</button>"
"<button data-demo='splash'>Scanning screen (10 s)</button>"
"<button data-demo='map'>Map (30 s)</button>"
"<button data-demo='stats'>Today's stats (10 s)</button>"
"<button data-demo='clock'>Night clock (10 s)</button>"
"</div>"
"</div>"
"<div class='card'>"
"<h2>Log</h2>"
"<div id='lb'></div>"
"</div>"
"</div>"

// ── Right: settings ─────────────────────────────────────────────────────────
"<div>"
"<div class='card'>"
"<h2>Display</h2>"
"<div class='f'><label for='display_brightness'>Brightness <small id='bv'></small></label>"
"<input type='range' min='5' max='255' id='display_brightness'></div>"
"<div class='f'><label for='screen_facing'>Window faces <small>(the way you look out; sets which way planes fly across)</small></label>"
"<select id='screen_facing'>"
"<option>N</option><option>NNE</option><option>NE</option><option>ENE</option>"
"<option>E</option><option>ESE</option><option>SE</option><option>SSE</option>"
"<option>S</option><option>SSW</option><option>SW</option><option>WSW</option>"
"<option>W</option><option>WNW</option><option>NW</option><option>NNW</option>"
"</select></div>"
"<div class='row'>"
"<div class='f'><label for='card_lead_seconds'>Card before landing (s) <small>(when an approach flies in)</small></label>"
"<input type='number' min='30' max='600' id='card_lead_seconds'></div>"
"<div class='f'><label for='interlude_seconds'>Break after a landing (s) <small>(0 = none)</small></label>"
"<input type='number' min='0' max='120' id='interlude_seconds'></div>"
"</div>"
"<div class='f'><label>Screens between planes <small>(each comes round in turn)</small></label>"
"<div class='btns'>"
"<label class='ck'><input type='checkbox' data-scr='1'>Map</label>"
"<label class='ck'><input type='checkbox' data-scr='2'>Arrivals</label>"
"<label class='ck'><input type='checkbox' data-scr='4'>Stats</label>"
"<label class='ck'><input type='checkbox' data-scr='8'>Weather</label>"
"</div></div>"
"<div class='f'><label for='display_cycle_seconds'>Minimum time per approach card (s)</label>"
"<input type='number' min='3' id='display_cycle_seconds'></div>"
"<label class='ck'><input type='checkbox' id='display_border'>Airline-coloured border</label>"
"<label class='ck'><input type='checkbox' id='display_nearest_only'><span>Nearest aircraft only <small>(one lookup per fetch; a new card when the nearest plane changes)</small></span></label>"
"<label class='ck'><input type='checkbox' id='display_flip'>Rotate 180&#176;</label>"
"</div>"

"<div class='card'>"
"<h2>Location</h2>"
"<div class='row'>"
"<div class='f'><label for='center_lat'>Latitude</label><input type='number' step='any' id='center_lat'></div>"
"<div class='f'><label for='center_lon'>Longitude</label><input type='number' step='any' id='center_lon'></div>"
"</div>"
"<div class='row'>"
"<div class='f'><label for='home_lat'>Home marker lat <small>(map; 0 = centre)</small></label><input type='number' step='any' id='home_lat'></div>"
"<div class='f'><label for='home_lon'>Home marker lon</label><input type='number' step='any' id='home_lon'></div>"
"</div>"
"<div class='row'>"
"<div class='f'><label for='radius_km'>Radius (km)</label><input type='number' step='1' min='1' id='radius_km'></div>"
"<div class='f'><label for='min_altitude_ft'>Ignore below (ft)</label><input type='number' step='1' min='0' placeholder='no filter' id='min_altitude_ft'></div>"
"</div>"
"</div>"

"<div class='card'>"
"<h2>Night mode</h2>"
"<label class='ck'><input type='checkbox' id='night_mode_enabled'><span>Dim at night <small>(UK time, GMT/BST automatic)</small></span></label>"
"<div class='row'>"
"<div class='f'><label for='night_start_time'>From</label><input type='time' id='night_start_time'></div>"
"<div class='f'><label for='night_end_time'>Until</label><input type='time' id='night_end_time'></div>"
"</div>"
"<div class='row'>"
"<div class='f'><label for='night_brightness'>Night brightness <small>(0 = off)</small></label><input type='number' min='0' max='255' id='night_brightness'></div>"
"</div>"
"</div>"

"<div class='card'>"
"<details>"
"<summary>Data sources &amp; advanced</summary>"
"<label class='ck'><input type='checkbox' id='use_community_feeds'><span>Live positions from adsb.lol / adsb.fi <small>(free, ~1 s fresh; OpenSky is the fallback)</small></span></label>"
"<div class='f'><label for='opensky_client_id'>OpenSky client ID</label>"
"<input id='opensky_client_id' autocomplete='off' spellcheck='false'></div>"
"<div class='f'><label for='opensky_client_secret'>OpenSky client secret</label>"
"<input type='password' id='opensky_client_secret' autocomplete='new-password' placeholder='leave blank to keep current'></div>"
"<div class='f'><label for='tar1090_host'>Local ADS-B receiver (tar1090) <small>(blank = OpenSky only)</small></label>"
"<input id='tar1090_host' autocomplete='off' spellcheck='false' placeholder='e.g. 192.168.1.10'></div>"
"<div class='f'><label for='aeroapi_key'>FlightAware AeroAPI key <small>(optional, paid)</small></label>"
"<input type='password' id='aeroapi_key' autocomplete='new-password' placeholder='leave blank to keep current'></div>"
"<label class='ck'><input type='checkbox' id='opensky_priority'>Use OpenSky first for routes <small>(after restart)</small></label>"
"<div class='row'>"
"<div class='f'><label for='fetch_interval_seconds'>OpenSky fetch (s)</label><input type='number' min='30' id='fetch_interval_seconds'></div>"
"<div class='f'><label for='local_fetch_interval_seconds'>Live feed / receiver fetch (s)</label><input type='number' min='1' id='local_fetch_interval_seconds'></div>"
"</div>"
"<div class='row'>"
"<div class='f'><label for='aeroapi_cache_ttl_seconds'>Route cache (s)</label><input type='number' min='60' id='aeroapi_cache_ttl_seconds'></div>"
"<div class='f'><label for='aeroapi_fail_cache_ttl_seconds'>Retry unknown routes after (s)</label><input type='number' min='30' id='aeroapi_fail_cache_ttl_seconds'></div>"
"</div>"
"<div class='btns'>"
"<button id='wifi'>Change Wi-Fi network&hellip;</button>"
"<button id='bak'>Back up settings</button>"
"<button id='res'>Restore settings&hellip;</button><input type='file' id='resf' accept='.json' style='display:none'>"
"<button id='rst'>Reset to defaults</button>"
"<button id='reboot'>Restart</button>"
"</div>"
"</details>"
"</div>"

"<div class='card'>"
"<h2>Airport</h2>"
"<div id='apt' class='f'><small>&nbsp;</small></div>"
"<div class='f'><label for='aps'>Watch another airport <small>(ready-made packs, downloaded from GitHub by this browser)</small></label>"
"<select id='aps'><option value=''>Loading the list&hellip;</option></select></div>"
"<div class='f'><label for='apf'>&hellip;or a pack file <small>(made with tools/airport_pack.py)</small></label>"
"<input type='file' id='apf' accept='.airport'></div>"
"<div class='btns'><button id='apu'>Install airport</button><button id='apc'>Centre search on airport</button>"
"<button id='apr'>Back to Heathrow</button><span id='ast' style='align-self:center;color:#8b949e;font-size:13px'></span></div>"
"</div>"
"<div class='card'>"
"<h2>Firmware</h2>"
"<div id='ver' class='f'><small>&nbsp;</small></div>"
"<div class='f'><label for='fw'>Install an update <small>(firmware.bin from a release or your own build)</small></label>"
"<input type='file' id='fw' accept='.bin'></div>"
"<div class='btns'><button id='upd'>Install update</button><span id='ust' style='align-self:center;color:#8b949e;font-size:13px'></span></div>"
"</div>"

"<div class='bar'><button class='go' id='save'>Save</button><span id='st' role='status'></span></div>"
"</div>"
"</div>"

"<script>"
"var $=function(id){return document.getElementById(id);};"
"var cur=0,lines=0,lb=$('lb');"
"function status(t,ms){$('st').textContent=t;if(ms)setTimeout(function(){$('st').textContent='';},ms);}"
"function post(url,body){return fetch(url,{method:'POST',headers:{'Content-Type':'application/json'},body:body?JSON.stringify(body):undefined});}"
"var toTime=function(m){var h=Math.floor(m/60),n=m%60;return(h<10?'0':'')+h+':'+(n<10?'0':'')+n;};"
"var toMin=function(t){var p=(t||'00:00').split(':');return parseInt(p[0])*60+(parseInt(p[1])||0);};"

"function load(){"
"fetch('/api/config').then(function(r){return r.json();}).then(function(d){"
"Object.keys(d).forEach(function(k){var e=$(k);if(!e)return;if(e.type==='checkbox')e.checked=!!d[k];else e.value=d[k];});"
"if(d.min_altitude_ft===-1)$('min_altitude_ft').value='';"
"$('night_start_time').value=toTime(d.night_start_minutes||0);"
"$('night_end_time').value=toTime(d.night_end_minutes||0);"
"['opensky_client_secret','aeroapi_key'].forEach(function(k){var e=$(k);e.value='';e.placeholder=d[k]==='***'?'set (leave blank to keep)':'not set';});"
"$('bv').textContent=d.display_brightness;"
"document.querySelectorAll('[data-scr]').forEach(function(e){e.checked=!!(d.screens&parseInt(e.dataset.scr));});"
"$('hdr').textContent=d.center_lat.toFixed(3)+', '+d.center_lon.toFixed(3)+' · '+d.radius_km+' km';"
"});"
"}"

"function save(){"
"var d={};"
"['center_lat','center_lon','radius_km','home_lat','home_lon'].forEach(function(k){d[k]=parseFloat($(k).value)||0;});"
"d.screens=0;document.querySelectorAll('[data-scr]').forEach(function(e){if(e.checked)d.screens|=parseInt(e.dataset.scr);});"
"['display_brightness','display_cycle_seconds','card_lead_seconds','interlude_seconds','night_brightness','fetch_interval_seconds',"
"'local_fetch_interval_seconds','aeroapi_cache_ttl_seconds','aeroapi_fail_cache_ttl_seconds'].forEach(function(k){d[k]=parseInt($(k).value);});"
"['display_nearest_only','display_border','display_flip','night_mode_enabled','opensky_priority','use_community_feeds'].forEach(function(k){d[k]=$(k).checked;});"
"d.screen_facing=$('screen_facing').value;"
"d.tar1090_host=$('tar1090_host').value;"
"var ma=$('min_altitude_ft').value;d.min_altitude_ft=ma===''?-1:parseInt(ma);"
"d.night_start_minutes=toMin($('night_start_time').value);"
"d.night_end_minutes=toMin($('night_end_time').value);"
"['opensky_client_id','opensky_client_secret','aeroapi_key'].forEach(function(k){var v=$(k).value;if(v.length)d[k]=v;});"
"status('Saving…');"
"post('/api/config',d).then(function(r){return r.json();})"
".then(function(j){status(j.ok?'Saved':'Error: '+(j.error||'?'),3000);load();})"
".catch(function(){status('Could not reach the board',4000);});"
"}"

// Brightness applies as you drag (debounced), without needing Save.
"var bt;$('display_brightness').addEventListener('input',function(e){"
"$('bv').textContent=e.target.value;clearTimeout(bt);"
"bt=setTimeout(function(){post('/api/config',{display_brightness:parseInt(e.target.value)});},250);});"

"$('save').addEventListener('click',save);"
"$('rst').addEventListener('click',function(){if(!confirm('Reset all settings to the firmware defaults?'))return;"
"post('/api/config/reset').then(function(){load();status('Reset to defaults',3000);});});"
"$('reboot').addEventListener('click',function(){if(!confirm('Restart the board now?'))return;"
"post('/api/restart').catch(function(){});status('Restarting…');setTimeout(function(){location.reload();},7000);});"
"$('wifi').addEventListener('click',function(){if(!confirm('Forget the saved Wi-Fi and restart as the Glideslope-Setup hotspot? You will need to join that network to set it up again.'))return;"
"post('/api/wifi/reset').catch(function(){});status('Wi-Fi cleared: join Glideslope-Setup');});"
"document.querySelectorAll('[data-demo]').forEach(function(b){b.addEventListener('click',function(){post('/api/demo/'+b.dataset.demo);});});"

"function poll(){if(document.hidden){setTimeout(poll,3000);return;}"
"fetch('/api/log?cursor='+cur).then(function(r){return r.json();}).then(function(d){"
"if(d.lines&&d.lines.length){var atEnd=lb.scrollHeight-lb.scrollTop<=lb.clientHeight+8;"
"d.lines.forEach(function(l){var v=document.createElement('div');v.textContent=l;lb.appendChild(v);"
"if(++lines>400)lb.removeChild(lb.firstChild);});cur=d.cursor;if(atEnd)lb.scrollTop=lb.scrollHeight;}"
"}).catch(function(){}).finally(function(){setTimeout(poll,1500);});"
"}"

// Exact panel frame (GET /api/frame, RGB565) drawn as LED dots. All polling
// is gentle (every request costs the board heap) and stops in a hidden tab.
"function pollFrame(){if(document.hidden){setTimeout(pollFrame,3000);return;}"
"fetch('/api/frame',{cache:'no-store'}).then(function(r){return r.arrayBuffer();}).then(function(ab){"
"var px=new Uint16Array(ab),g=$('dmp').getContext('2d');if(px.length<8192)return;"
"g.fillStyle='#050608';g.fillRect(0,0,512,256);"
"for(var i=0;i<8192;i++){var v=px[i],x=(i&127)*4,y=(i>>7)*4;"
"if(!v){g.fillStyle='#14171d';g.fillRect(x+1,y+1,2,2);continue;}"
"var r=(v>>11)&31,gg=(v>>5)&63,b=v&31;"
"g.fillStyle='rgb('+((r<<3)|(r>>2))+','+((gg<<2)|(gg>>4))+','+((b<<3)|(b>>2))+')';g.fillRect(x,y,3,3);}"
"}).catch(function(){}).finally(function(){setTimeout(pollFrame,800);});"
"}"

"function pollNow(){if(document.hidden){setTimeout(pollNow,3000);return;}"
"fetch('/api/display').then(function(r){return r.json();}).then(function(d){"
"var n=$('now');if(!d.active){n.textContent='Scanning for traffic';return;}"
"n.innerHTML='';var b=document.createElement('b');b.textContent=d.flight+(d.flight!==d.ident?' ('+d.ident+')':'');n.appendChild(b);"
"n.appendChild(document.createTextNode('  '+(d.origin||'?')+' → '+(d.dest||'?')+'  ·  '+(d.aircraft_name||'')+"
"'  ·  '+d.altitude_ft+' ft'+(d.registration?'  ·  '+d.registration:'')));"
"}).catch(function(){}).finally(function(){setTimeout(pollNow,3000);});"
"}"

"fetch('/api/status').then(function(r){return r.json();}).then(function(s){"
"var v=$('ver').firstChild;v.textContent='Running '+s.version+' (built '+s.built+')'+(s.metar?' · '+s.metar:'');});"
"$('upd').addEventListener('click',function(){var f=$('fw').files[0],u=$('ust');"
"if(!f){u.textContent='Choose a .bin file first';return;}"
"if(!confirm('Install '+f.name+' and restart the board?'))return;"
"var x=new XMLHttpRequest();x.open('POST','/api/update');"
"x.upload.onprogress=function(e){if(e.lengthComputable)u.textContent='Uploading '+Math.round(e.loaded*100/e.total)+'%';};"
"x.onload=function(){var j={};try{j=JSON.parse(x.responseText);}catch(e){}"
"if(j.ok){u.textContent='Installed, restarting…';setTimeout(function(){location.reload();},12000);}"
"else u.textContent='Failed: '+(j.error||x.status);};"
"x.onerror=function(){u.textContent='Upload failed (connection lost)';};"
"x.setRequestHeader('Content-Type','application/octet-stream');x.send(f);});"
"$('bak').addEventListener('click',function(){fetch('/api/config').then(function(r){return r.text();}).then(function(t){"
"var a=document.createElement('a');a.href=URL.createObjectURL(new Blob([t],{type:'application/json'}));"
"a.download='glideslope-settings.json';a.click();status('Saved (API secrets are not included)',4000);});});"
"$('res').addEventListener('click',function(){$('resf').click();});"
"$('resf').addEventListener('change',function(e){var f=e.target.files[0];if(!f)return;f.text().then(function(t){"
"var d;try{d=JSON.parse(t);}catch(x){status('Not a settings file',4000);return;}"
"if(!confirm('Restore settings from '+f.name+'?'))return;"
"post('/api/config',d).then(function(r){return r.json();}).then(function(j){status(j.ok?'Restored':'Error',3000);load();});});e.target.value='';});"
"var st={};fetch('/api/status').then(function(r){return r.json();}).then(function(s){st=s;"
"$('apt').firstChild.textContent='Watching '+s.airport_name+' ('+s.airport+')'+(s.airport_pack?' from an airport pack':', built in');});"
"var PACKS='https://raw.githubusercontent.com/SamHoughton/Glideslope/main/packs/';"
"fetch(PACKS+'index.json',{cache:'no-store'}).then(function(r){if(!r.ok)throw 0;return r.json();}).then(function(d){"
"var sel=$('aps');sel.innerHTML='<option value=\"\">Choose an airport&hellip;</option>';"
"d.packs.forEach(function(p){var o=document.createElement('option');o.value=p.file;"
"o.textContent=p.name+' ('+p.icao+(p.iata?' / '+p.iata:'')+')'+(p.city?', '+p.city:'')+(p.country?', '+p.country:'');sel.appendChild(o);});"
"}).catch(function(){$('aps').innerHTML='<option value=\"\">Could not reach GitHub: choose a pack file below</option>';});"
"function sendPack(blob,label){var u=$('ast');"
"if(!confirm('Install '+label+' and restart the board?'))return;u.textContent='Installing '+label+'…';"
"fetch('/api/airport',{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:blob}).then(function(r){return r.json();})"
".then(function(j){if(j.ok){u.textContent='Installed '+j.airport+', restarting…';setTimeout(function(){location.reload();},9000);}"
"else u.textContent='Failed: '+j.error;}).catch(function(){u.textContent='Upload failed';});}"
"$('apu').addEventListener('click',function(){var f=$('apf').files[0],v=$('aps').value,u=$('ast');"
"if(f){sendPack(f,f.name);return;}"
"if(!v){u.textContent='Choose an airport or a pack file first';return;}"
"u.textContent='Downloading '+v+'…';"
"fetch(PACKS+v).then(function(r){if(!r.ok)throw 0;return r.blob();}).then(function(b){sendPack(b,$('aps').selectedOptions[0].textContent);})"
".catch(function(){u.textContent='Could not download '+v;});});"
"$('apr').addEventListener('click',function(){if(!confirm('Go back to the built-in Heathrow and restart?'))return;"
"post('/api/airport/reset').catch(function(){});$('ast').textContent='Restarting…';setTimeout(function(){location.reload();},9000);});"
"$('apc').addEventListener('click',function(){if(!st.airport_lat)return;"
"if(!confirm('Set the search centre to '+st.airport_name+' ('+st.airport_lat.toFixed(4)+', '+st.airport_lon.toFixed(4)+')?'))return;"
"post('/api/config',{center_lat:st.airport_lat,center_lon:st.airport_lon}).then(function(){load();$('ast').textContent='Search centred on '+st.airport_name;});});"
"load();poll();pollFrame();pollNow();"
"</script>"
"</body>"
"</html>";

const size_t kHtmlPageLen = sizeof(kHtmlPage) - 1;  // exclude null terminator
