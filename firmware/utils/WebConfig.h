#pragma once

/*
Purpose: Minimal HTTP server (port 80) that serves the configuration web UI
and a streaming serial log panel.  Built on WiFiServer/WiFiClient to avoid
any framework library dependency beyond what the project already uses.

Routes:
  GET  /                  — single-page UI (HTML)
  GET  /api/config        — current g_config as JSON
  POST /api/config        — update g_config from JSON body; saves to NVS
  POST /api/config/reset  — reset g_config to compile-time defaults
  GET  /api/log?cursor=N  — log lines since sequence number N + next cursor
  GET  /api/frame         — raw 128x64 RGB565 frame currently on the panel
  GET  /api/status        — version, uptime, memory, weather
  POST /api/update        — new firmware image (raw .bin body); installs and restarts
  POST /api/airport       — airport pack (tools/airport_pack.py); installs and restarts
  POST /api/airport/reset — back to the built-in Heathrow; restarts

Usage:
  Call g_webConfig.begin() once after WiFi connects.
  Call g_webConfig.loop() on every iteration of loop().
*/

#include <Arduino.h>
#include <WiFiClient.h>
#include <WiFiServer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "models/FlightInfo.h"

// The release build passes the git tag as -DGLIDESLOPE_VERSION_RAW=v1.2.0;
// "dev" for local builds.
#define GS_STR2(x) #x
#define GS_STR(x) GS_STR2(x)
#ifdef GLIDESLOPE_VERSION_RAW
#define GLIDESLOPE_VERSION GS_STR(GLIDESLOPE_VERSION_RAW)
#else
#define GLIDESLOPE_VERSION "dev"
#endif

class WebConfig
{
public:
    void begin(uint16_t port = 80);
    void loop();

    // Called from main.cpp after each displayFlights() tick so the web preview
    // always reflects the card currently on screen.  Pass nullptr when the
    // flight list is empty (shows no-flight state in the browser).
    void setCurrentFlight(const FlightInfo *f);

    // Latest raw METAR, reported by /api/status (main loop).
    void setWeather(const char *metar);
    void setRunways(const char *arrivals, const char *departures);

    // Requests accepted so far (heartbeat diagnostics).
    uint32_t requestsServed() const { return _requests; }

private:
    WiFiServer _server{80};
    static constexpr uint32_t kMinHeapToServe = 40000;

    // "Now showing" JSON, built on the main loop by setCurrentFlight() and read
    // by the web task; guarded by _displayMutex.
    String                  _displayJson = "{\"active\":false}";
    String                  _metar;               // guarded by _displayMutex
    char                    _rwyArr[12] = "", _rwyDep[12] = "";   // guarded by _displayMutex
    SemaphoreHandle_t       _displayMutex = nullptr;
    volatile uint32_t       _requests = 0;

    // Parsed HTTP request context
    struct Req {
        String method;
        String path;
        String query;
        String body;
        int    contentLength = 0;
    };

    // Helpers
    static void   sendHttp(WiFiClient &c, int code,
                            const char *contentType, const String &body);
    static void   sendHtmlDirect(WiFiClient &c, const char *html, size_t len);
    static void   sendChunked(WiFiClient &c, const uint8_t *data, size_t len, size_t chunk);
    static String qparam(const String &query, const char *key);

    // Route handlers
    void handleRoot(WiFiClient &c);
    void handleGetConfig(WiFiClient &c);
    void handlePostConfig(WiFiClient &c, const Req &r);
    void handleResetConfig(WiFiClient &c);
    void handleRestart(WiFiClient &c);
    void handleWifiReset(WiFiClient &c);
    void handleGetLog(WiFiClient &c, const Req &r);
    void handleGetDisplay(WiFiClient &c);
    void handleGetFrame(WiFiClient &c);
    void handleGetStatus(WiFiClient &c);
    void handleUpdate(WiFiClient &c, int length);
    void handleAirport(WiFiClient &c, int length);
    void handleAirportReset(WiFiClient &c);
};

extern WebConfig g_webConfig;
