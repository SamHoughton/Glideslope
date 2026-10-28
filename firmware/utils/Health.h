#pragma once
/*
Purpose: The board's health over time (8.2), for the web page's Health tab
(GET /api/health): why it last restarted and the restarts before (kept in
flash with when they happened), its memory over the last 24 hours (the
lowest free and the smallest largest-block in each 15 minutes: the
fragmentation the soaks watch for), and how the feed and the weather are
doing. Alongside: Wi-Fi signal, storage, the chip's temperature.

A restart the firmware chooses (an update, the web page's button, the
self-heal for a fragmented heap) leaves its reason in memory that survives
the restart, so the list says "self-heal: memory fragmented" rather than
just "restart".

Memory: 192 bytes of history; the restart list is read from flash when asked.
*/
#include <Arduino.h>

class WiFiClient;

namespace Health
{
    void begin();   // at boot, after StageTrace::reportAtBoot()
    // Main loop, every pass: samples the heap once a minute and logs the
    // boot (with its time) once the clock is set.
    void loop();
    // Just before a chosen ESP.restart(): what it was for.
    void noteRestart(const char *why);
    // Before a self-heal restart (9.1.4): the reason, the heap and the last
    // log lines to /log/blackbox.txt, so what led up to it can be read
    // afterwards (GET /api/blackbox). One file, the latest.
    void saveBlackBox(const char *why);
    void writeBlackBox(WiFiClient &c);
    // Each position fetch and weather report, ok or not.
    void noteFetch(bool ok);
    void noteWeather(bool ok);
    // The JSON for /api/health.
    void json(String &out);
}
