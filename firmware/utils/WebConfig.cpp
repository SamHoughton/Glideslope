/*
Purpose: WebConfig — minimal HTTP server, config API, and log streaming.
Uses WiFiServer/WiFiClient (already a project dependency) instead of the
WebServer library to avoid framework include-path issues.
*/
#include "utils/Notify.h"
#include "utils/WebConfig.h"
#include "utils/TelnetLogger.h"
#include "utils/WifiProvisioner.h"
#include "utils/StageTrace.h"
#include "utils/ImageCheck.h"
#include "utils/FirmwareSig.h"
#include "config/RuntimeConfig.h"
#include "config/Airport.h"
#include <LittleFS.h>
#include "adapters/NeoMatrixDisplay.h"
#include <ArduinoJson.h>
#include <WiFi.h>
#include <Update.h>
#include <Preferences.h>
#include <esp_task_wdt.h>
#include "utils/HeapWatch.h"
#include "utils/HttpRequest.h"
#include "utils/TlsArena.h"
#include <vector>

WebConfig g_webConfig;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void WebConfig::loadToken()
{
    Preferences p;
    p.begin("fw_auth", false);
    String t = p.getString("token", "");
    if (t.length() != 9)
    {
        // XXXX-XXXX from letters and digits that can't be mixed up on the panel.
        static const char kChars[] = "ACDEFGHJKMNPQRTUVWXY34679";
        char buf[10];
        for (int i = 0; i < 9; ++i) buf[i] = i == 4 ? '-' : kChars[esp_random() % (sizeof(kChars) - 1)];
        buf[9] = '\0';
        t = buf;
        p.putString("token", t);
        Log.println("WebConfig: made the web page code (shown on the panel when asked)");
    }
    p.end();
    strlcpy(_token, t.c_str(), sizeof(_token));
}

String WebConfig::header(const String &raw, const char *name)
{
    return HttpRequest::header(raw, name);
}


bool WebConfig::hostAllowed(const String &hostPort)
{
    String host = hostPort;
    const int colon = host.indexOf(':');
    if (colon >= 0) host = host.substring(0, colon);
    host.toLowerCase();
    return host == "glideslope.local" || host == "glideslope" ||
           host == WiFi.localIP().toString() || host == WiFi.softAPIP().toString();
}

bool WebConfig::needsToken(const String &p)
{
    return p == "/api/update" || p == "/api/airport" || p == "/api/airport/reset" || p == "/api/config" ||
           p == "/api/config/reset" || p == "/api/restart" || p == "/api/wifi/reset" ||
           p == "/api/coredump/erase" || p == "/api/notify/test";
}

void WebConfig::begin(uint16_t port)
{
    if (!_displayMutex) _displayMutex = xSemaphoreCreateMutex();
    loadToken();
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
            if (raw.length() > 4096) { client.stop(); return; }   // no request needs headers this long
        }
        if (!headersRead) delay(1);
    }
    if (!headersRead) { client.stop(); return; }

    // ── Parse request line ─────────────────────────────────────────────────
    HttpRequest::Line line;
    if (!HttpRequest::parseLine(raw, line)) { client.stop(); return; }
    Req r;
    r.method = line.method;
    r.path   = line.path;
    r.query  = line.query;

    // ── Who is asking ──────────────────────────────────────────────────────
    if (!hostAllowed(header(raw, "Host")))
    {
        sendHttp(client, 403, "text/plain", "Unknown host: use http://glideslope.local/ or the board's address");
        client.stop();
        return;
    }
    if (r.method == "POST")
    {
        const String origin = header(raw, "Origin");
        const bool sameOrigin = !origin.length() || origin.equalsIgnoreCase("http://" + header(raw, "Host"));
        if (!sameOrigin || header(raw, "X-Glideslope") != "1")
        {
            sendHttp(client, 403, "application/json", "{\"ok\":false,\"error\":\"not from this board's page\"}");
            client.stop();
            return;
        }
        if (r.path == "/api/token/show")
        {
            // At most every 20 s: someone at the board reads it off the panel.
            if (!_tokenShownMs || millis() - _tokenShownMs > 20000)
            {
                _tokenShownMs = millis();
                char msg[20];
                snprintf(msg, sizeof(msg), "CODE %s", _token);
                requestPanelMessage(msg, 30000);
            }
            sendHttp(client, 200, "application/json", "{\"ok\":true}");
            client.stop();
            return;
        }
        r.allowUnsigned = header(raw, "X-Glideslope-Unsigned") == "1";
        if (needsToken(r.path) && header(raw, "X-Glideslope-Token") != _token)
        {
            sendHttp(client, 401, "application/json", "{\"ok\":false,\"error\":\"code needed\"}");
            client.stop();
            return;
        }
    }

    // ── Read POST body (Content-Length driven) ─────────────────────────────
    if (r.method == "POST")
    {
        const long cl = HttpRequest::contentLength(raw);
        if (cl >= 0)
        {
            int bodyLen = (int)cl;
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
    else if (r.path == "/api/frame"         && r.method == "GET")  handleGetFrame(client, r);
    else if (r.path == "/api/status"        && r.method == "GET")  handleGetStatus(client);
    else if (r.path == "/api/update"        && r.method == "POST") handleUpdate(client, r.contentLength, r.allowUnsigned);
    else if (r.path == "/api/airport"       && r.method == "POST") handleAirport(client, r.contentLength);
    else if (r.path == "/api/airport/reset" && r.method == "POST") handleAirportReset(client);
    else if (r.path == "/api/demo/showcase" && r.method == "POST") { requestShowcase(qparam(r.query, "look").c_str()); sendHttp(client, 200, "application/json", "{\"ok\":true}"); }
    else if (r.path == "/api/demo/takeoff"  && r.method == "POST") { requestTakeoffDemo(); sendHttp(client, 200, "application/json", "{\"ok\":true}"); }
    else if (r.path == "/api/demo/squawk"   && r.method == "POST") { requestAlertDemo(); sendHttp(client, 200, "application/json", "{\"ok\":true}"); }
    else if (r.path == "/api/demo/sky"      && r.method == "POST") { const String look = qparam(r.query, "look"); requestSkyPreview(look.length() ? look.c_str() : "night"); sendHttp(client, 200, "application/json", "{\"ok\":true}"); }
    else if (r.path == "/api/demo/runway"   && r.method == "POST") { requestRunwayChangeDemo(); sendHttp(client, 200, "application/json", "{\"ok\":true}"); }
    else if (r.path == "/api/demo/weather"  && r.method == "POST") { const String look = qparam(r.query, "look"); if (look.length()) requestSkyPreview(look.c_str(), false); requestScreenPreview(4, 10000); sendHttp(client, 200, "application/json", "{\"ok\":true}"); }
    else if (r.path == "/api/demo/arrivals" && r.method == "POST") { requestScreenPreview(3, 10000); sendHttp(client, 200, "application/json", "{\"ok\":true}"); }
    else if (r.path == "/api/demo/sprites"  && r.method == "POST") { requestSpriteGallery(10000); sendHttp(client, 200, "application/json", "{\"ok\":true}"); }
    else if (r.path == "/api/demo/splash"   && r.method == "POST") { requestSplashPreview(10000); sendHttp(client, 200, "application/json", "{\"ok\":true}"); }
    else if (r.path == "/api/demo/map"      && r.method == "POST") { requestMapPreview(30000); sendHttp(client, 200, "application/json", "{\"ok\":true}"); }
    else if (r.path == "/api/demo/stats"    && r.method == "POST") { requestScreenPreview(1, 24000); sendHttp(client, 200, "application/json", "{\"ok\":true}"); }
    else if (r.path == "/api/demo/overhead" && r.method == "POST") { requestOverheadDemo(); sendHttp(client, 200, "application/json", "{\"ok\":true}"); }
    else if (r.path == "/api/demo/globe"    && r.method == "POST") { requestScreenPreview(6, 12000); sendHttp(client, 200, "application/json", "{\"ok\":true}"); }
    else if (r.path == "/api/coredump"      && r.method == "GET")
    {
        static char text[300];
        String body = "{\"present\":false}";
        if (StageTrace::coreDump(text, sizeof(text)))
        {
            JsonDocument d;
            d["present"] = true;
            d["summary"] = text;
            // The build that crashed: this one, or one since replaced (the
            // backtrace then needs that build's firmware.elf to decode).
            char elf[24];
            const bool same = StageTrace::coreDumpFromThisBuild(elf, sizeof(elf));
            d["built"]   = same ? __DATE__ " " __TIME__ : "an earlier build";
            d["elf"]     = elf;
            d["this_build"] = same;
            body = "";
            serializeJson(d, body);
        }
        sendHttp(client, 200, "application/json", body);
    }
    else if (r.path == "/api/coredump/erase" && r.method == "POST") { StageTrace::eraseCoreDump(); sendHttp(client, 200, "application/json", "{\"ok\":true}"); }
    else if (r.path == "/api/notify/test"   && r.method == "POST")
    {
        Notify::post(Notify::Test, 3, "white_check_mark", "Glideslope", "Test from %s: notifications are working.", g_airport.name);
        sendHttp(client, 200, "application/json", g_config.ntfy_topic[0] ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"no topic set\"}");
    }
    else if (r.path == "/api/demo/holding"  && r.method == "POST") { requestScreenPreview(5, 10000); sendHttp(client, 200, "application/json", "{\"ok\":true}"); }
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
    return HttpRequest::qparam(query, key);
}

// ---------------------------------------------------------------------------
// Route handlers
// ---------------------------------------------------------------------------

// The page: web/index.html, turned into a header at build time
// (scripts/gen_webpage.py).
#include "utils/WebPage.h"

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
    doc["night_follow_sun"]              = g_config.night_follow_sun;
    doc["route_cities"]                  = g_config.route_cities;
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
    doc["ntfy_topic"]            = g_config.ntfy_topic;
    doc["ntfy_token"]            = g_config.ntfy_token[0] ? "***" : "";
    doc["overhead_km"]           = g_config.overhead_km;
    doc["overhead_all"]          = g_config.overhead_all;
    doc["watchlist"]             = g_config.watchlist;
    doc["mqtt_host"]             = g_config.mqtt_host;
    doc["mqtt_port"]             = g_config.mqtt_port;
    doc["mqtt_user"]             = g_config.mqtt_user;
    doc["mqtt_pass"]             = g_config.mqtt_pass[0] ? "***" : "";
    doc["notify_mask"]           = g_config.notify_mask;
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

    // Built on a copy and published in one go (configCommit), so the main
    // loop and display never see half the new settings.
    RuntimeConfig next = configSnapshot();

    // ArduinoJson "| current" idiom: keeps existing value when key is absent
    {
        const char *v = doc["tar1090_host"] | next.tar1090_host;
        strncpy(next.tar1090_host, v, sizeof(next.tar1090_host) - 1);
        next.tar1090_host[sizeof(next.tar1090_host) - 1] = '\0';
    }
    next.center_lat   = doc["center_lat"]   | next.center_lat;
    next.center_lon   = doc["center_lon"]   | next.center_lon;
    next.home_lat     = doc["home_lat"]     | next.home_lat;
    next.home_lon     = doc["home_lon"]     | next.home_lon;
    next.radius_km    = doc["radius_km"]    | next.radius_km;
    next.min_altitude_ft = doc["min_altitude_ft"] | next.min_altitude_ft;

    next.display_brightness = (uint8_t)(doc["display_brightness"] | (int)next.display_brightness);
    next.text_color_r       = (uint8_t)(doc["text_color_r"]       | (int)next.text_color_r);
    next.text_color_g       = (uint8_t)(doc["text_color_g"]       | (int)next.text_color_g);
    next.text_color_b       = (uint8_t)(doc["text_color_b"]       | (int)next.text_color_b);

    next.display_nearest_only         = doc["display_nearest_only"]         | next.display_nearest_only;
    next.display_border               = doc["display_border"]               | next.display_border;
    next.show_flight_number_with_logo = doc["show_flight_number_with_logo"] | next.show_flight_number_with_logo;
    next.show_aircraft_type           = doc["show_aircraft_type"]           | next.show_aircraft_type;
    next.show_aircraft_registration   = doc["show_aircraft_registration"]   | next.show_aircraft_registration;
    next.swap_aircraft_type_reg       = doc["swap_aircraft_type_reg"]       | next.swap_aircraft_type_reg;

    const char *facing = doc["screen_facing"] | next.screen_facing;
    strncpy(next.screen_facing, facing, sizeof(next.screen_facing) - 1);
    next.screen_facing[sizeof(next.screen_facing) - 1] = '\0';

    next.display_flip          = doc["display_flip"]          | next.display_flip;
    next.night_mode_enabled    = doc["night_mode_enabled"]    | next.night_mode_enabled;
    next.night_follow_sun      = doc["night_follow_sun"]      | next.night_follow_sun;
    next.route_cities          = doc["route_cities"]          | next.route_cities;
    next.night_start_minutes   = (uint16_t)(doc["night_start_minutes"] | (int)next.night_start_minutes);
    next.night_end_minutes     = (uint16_t)(doc["night_end_minutes"]   | (int)next.night_end_minutes);
    next.night_brightness      = (uint8_t)(doc["night_brightness"]     | (int)next.night_brightness);
    next.utc_offset_minutes    = doc["utc_offset_minutes"]    | next.utc_offset_minutes;

    next.opensky_priority              = doc["opensky_priority"]              | next.opensky_priority;
    next.use_community_feeds           = doc["use_community_feeds"]           | next.use_community_feeds;
    next.notify_mask                   = doc["notify_mask"]                   | next.notify_mask;
    next.overhead_km                   = doc["overhead_km"]                   | next.overhead_km;
    if (next.overhead_km < 0) next.overhead_km = 0;
    if (next.overhead_km > 20) next.overhead_km = 20;
    next.overhead_all                  = doc["overhead_all"]                  | next.overhead_all;
    next.mqtt_port                     = (uint16_t)(doc["mqtt_port"] | (int)next.mqtt_port);
    if (doc["watchlist"].is<const char *>()) strlcpy(next.watchlist, doc["watchlist"].as<const char *>(), sizeof(next.watchlist));
    if (doc["mqtt_host"].is<const char *>())
    {
        String h = doc["mqtt_host"].as<const char *>();
        h.trim();
        strlcpy(next.mqtt_host, h.c_str(), sizeof(next.mqtt_host));
    }
    if (doc["mqtt_user"].is<const char *>()) strlcpy(next.mqtt_user, doc["mqtt_user"].as<const char *>(), sizeof(next.mqtt_user));
    {
        const char *v = doc["mqtt_pass"] | "";
        if (strcmp(v, "***") != 0 && doc["mqtt_pass"].is<const char *>())
            strlcpy(next.mqtt_pass, v, sizeof(next.mqtt_pass));   // "" clears it
    }
    if (doc["ntfy_topic"].is<const char *>())
    {
        // Trimmed: a topic pasted with a stray space would silently go nowhere.
        String t = doc["ntfy_topic"].as<const char *>();
        t.trim();
        strlcpy(next.ntfy_topic, t.c_str(), sizeof(next.ntfy_topic));
    }
    {
        const char *v = doc["ntfy_token"] | "";
        if (strcmp(v, "***") != 0 && doc["ntfy_token"].is<const char *>())
            strlcpy(next.ntfy_token, v, sizeof(next.ntfy_token));   // "" clears it
    }

    next.fetch_interval_seconds        = doc["fetch_interval_seconds"]        | next.fetch_interval_seconds;
    next.local_fetch_interval_seconds  = doc["local_fetch_interval_seconds"]  | next.local_fetch_interval_seconds;
    next.display_cycle_seconds         = doc["display_cycle_seconds"]         | next.display_cycle_seconds;
    next.card_lead_seconds             = doc["card_lead_seconds"]             | next.card_lead_seconds;
    next.interlude_seconds             = doc["interlude_seconds"]             | next.interlude_seconds;
    next.screens                       = (uint8_t)(doc["screens"]             | (int)next.screens);
    next.aeroapi_cache_ttl_seconds     = doc["aeroapi_cache_ttl_seconds"]      | next.aeroapi_cache_ttl_seconds;
    next.aeroapi_fail_cache_ttl_seconds = doc["aeroapi_fail_cache_ttl_seconds"] | next.aeroapi_fail_cache_ttl_seconds;

    // API keys — only update when the posted value is non-empty and not the
    // mask sentinel "***".  Empty / absent = keep the current value intact.
    {
        const char *v = doc["opensky_client_id"] | "";
        if (strlen(v) > 0 && strcmp(v, "***") != 0) {
            strncpy(next.opensky_client_id, v, sizeof(next.opensky_client_id) - 1);
            next.opensky_client_id[sizeof(next.opensky_client_id) - 1] = '\0';
        }
    }
    {
        const char *v = doc["opensky_client_secret"] | "";
        if (strlen(v) > 0 && strcmp(v, "***") != 0) {
            strncpy(next.opensky_client_secret, v, sizeof(next.opensky_client_secret) - 1);
            next.opensky_client_secret[sizeof(next.opensky_client_secret) - 1] = '\0';
        }
    }
    {
        const char *v = doc["aeroapi_key"] | "";
        if (strlen(v) > 0 && strcmp(v, "***") != 0) {
            strncpy(next.aeroapi_key, v, sizeof(next.aeroapi_key) - 1);
            next.aeroapi_key[sizeof(next.aeroapi_key) - 1] = '\0';
        }
    }

    configCommit(next);
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
    ImageCheck::keep("restart requested");
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
    ImageCheck::keep("restart requested");
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
    doc["image"]          = ImageCheck::state();
    doc["heap_free"]      = ESP.getFreeHeap();
    doc["heap_max_block"] = ESP.getMaxAllocHeap();
    doc["heap_min_free"]  = ESP.getMinFreeHeap();
    doc["stack_web_free"] = uxTaskGetStackHighWaterMark(nullptr);   // this task: the web server
    doc["tls_arena"] = String(TlsArena::served()) + " served, " + TlsArena::overflowed() + " to the heap" + (TlsArena::slotsFree() ? "" : ", in use");
    doc["display_frames"] = displayFramesDrawn();
    doc["web_requests"]   = (uint32_t)_requests;
    if (_displayMutex) xSemaphoreTake(_displayMutex, portMAX_DELAY);
    doc["metar"]          = _metar;
    doc["runways_arr"]    = _rwyArr;
    doc["runways_dep"]    = _rwyDep;
    doc["holding"]        = _holding;
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

void WebConfig::setRunways(const char *arrivals, const char *departures)
{
    if (_displayMutex) xSemaphoreTake(_displayMutex, portMAX_DELAY);
    strlcpy(_rwyArr, arrivals, sizeof(_rwyArr));
    strlcpy(_rwyDep, departures, sizeof(_rwyDep));
    if (_displayMutex) xSemaphoreGive(_displayMutex);
}

void WebConfig::setHolding(const char *summary)
{
    if (_displayMutex) xSemaphoreTake(_displayMutex, portMAX_DELAY);
    strlcpy(_holding, summary, sizeof(_holding));
    if (_displayMutex) xSemaphoreGive(_displayMutex);
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
void WebConfig::handleUpdate(WiFiClient &c, int length, bool allowUnsigned)
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
    struct Updating { Updating() { g_updating = true; } ~Updating() { g_updating = false; g_feedPaused = false; } } updating;
    // Wait (up to 8 s) for the main loop to finish any fetch and close the
    // feed's connection, so the upload has the network and memory to
    // itself; the sender waits in the meantime.
    for (const unsigned long t0 = millis(); !g_feedPaused && millis() - t0 < 8000;)
    {
        esp_task_wdt_reset();
        delay(20);
    }
    requestPanelMessage("UPDATING");
    // Signature check as the image streams in (utils/FirmwareSig.h).
    static FirmwareSig sig;   // ~200 bytes: not on the web task's stack
    sig.begin(length);

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
            if (!c.connected() || millis() - lastData > 20000) break;
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
        sig.update(buf, n);
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
    const FirmwareSig::Result verdict = sig.finish();
    if (verdict == FirmwareSig::Bad) { Update.abort(); fail("signature does not match: not a genuine release"); return; }
    if (verdict == FirmwareSig::Unsigned && !allowUnsigned) { Update.abort(); fail("unsigned"); return; }
    if (!Update.end(true)) { fail(Update.errorString()); return; }

    Log.printf("Update: installed (%s), restarting\n", verdict == FirmwareSig::Signed ? "signed release" : "unsigned build");
    requestPanelMessage("RESTARTING");
    sendHttp(c, 200, "application/json", "{\"ok\":true}");
    c.flush();
    c.stop();
    delay(500);
    ImageCheck::keep("restart requested");
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
    ImageCheck::keep("restart requested");
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
    ImageCheck::keep("restart requested");
    ESP.restart();
}

// The 128x64 RGB565 frame currently on the panel (little-endian, row-major).
// With ?z=1, run-length encoded: [count 1-255][pixel lo][pixel hi] per run.
// Most of a frame is black or flat colour, so it is usually 1-4 KB instead of
// 16 KB, which keeps the page's live mirror smooth while the panel animates.
void WebConfig::handleGetFrame(WiFiClient &c, const Req &r)
{
    const size_t len = FrameCanvas::W * FrameCanvas::H * sizeof(uint16_t);
    // While the code is on the panel the mirror shows nothing: reading it off
    // the panel is the proof of being at the board.
    if (_tokenShownMs && millis() - _tokenShownMs < 32000)
    {
        c.print("HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nCache-Control: no-store\r\n"
                "Connection: close\r\n\r\n");
        uint8_t blank[3 * 33];
        for (int k = 0; k < 33; ++k) { blank[3 * k] = k < 32 ? 255 : 32; blank[3 * k + 1] = blank[3 * k + 2] = 0; }
        if (qparam(r.query, "z") == "1") sendChunked(c, blank, sizeof(blank), sizeof(blank));
        else { static const uint8_t zeros[512] = {}; for (size_t n = 0; n < len; n += sizeof(zeros)) sendChunked(c, zeros, sizeof(zeros), sizeof(zeros)); }
        return;
    }
    if (qparam(r.query, "z") == "1")
    {
        // No length up front (the frame can change while it is encoded):
        // the reply ends when the connection closes.
        // X-Show-Ms: the showcase time this frame was drawn at (recording).
        c.printf("HTTP/1.1 200 OK\r\n"
                 "Content-Type: application/octet-stream\r\n"
                 "Cache-Control: no-store\r\n"
                 "X-Show-Ms: %lu\r\n"
                 "Connection: close\r\n\r\n", (unsigned long)showcaseFrameMs());
        const uint16_t *px = g_shownFrame.pixels();
        const int n = FrameCanvas::W * FrameCanvas::H;
        uint8_t buf[768];
        size_t used = 0;
        for (int i = 0; i < n; )
        {
            const uint16_t v = px[i];
            int run = 1;
            while (i + run < n && run < 255 && px[i + run] == v) ++run;
            buf[used++] = (uint8_t)run;
            buf[used++] = v & 0xFF;
            buf[used++] = v >> 8;
            i += run;
            if (used + 3 > sizeof(buf) || i >= n)
            {
                sendChunked(c, buf, used, used);
                used = 0;
            }
        }
        return;
    }
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
    static char s_json[3 * 1024];
    StageTrace::mark(StageTrace::Web, StageTrace::WebHandle, 82);
    size_t n = Log.linesJson(cursor, 24, s_json, sizeof(s_json));
    if (n == 0) n = snprintf(s_json, sizeof(s_json), "{\"cursor\":%lu,\"lines\":[]}", (unsigned long)cursor);
    StageTrace::mark(StageTrace::Web, StageTrace::WebHandle, 83);
    c.printf("HTTP/1.1 200 OK\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %u\r\n"
             "Connection: close\r\n\r\n",
             (unsigned)n);
    sendChunked(c, (const uint8_t *)s_json, n, 1024);
}

