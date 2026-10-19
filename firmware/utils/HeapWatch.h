#pragma once
/*
Purpose: Find what pushes the heap lowest. Call heapCheckpoint("step") after
each memory-hungry step (TLS fetches, JSON parsing, web requests); when the
all-time minimum free heap has dropped since the last checkpoint, the step
just finished is logged as the culprit.
*/
#include <Arduino.h>
#include "utils/TelnetLogger.h"

// Whether there is room for a TLS connection now. The framework's mbedTLS is
// built with fixed 16 KB in/out buffers, so a handshake briefly needs 60-65
// KB of heap whatever the request; starting one with less free risks
// starving everything else (the web server, Wi-Fi). Callers defer instead:
// weather updates later, a route appears a fetch or two later.
// True while the main loop is fetching (positions, routes, weather): the web
// server takes no new requests then, so a TLS handshake never competes with
// a web reply for heap. Set for a scope with NetBusy.
inline volatile bool g_netBusy = false;
// A firmware upload is streaming in (web task): the main loop skips its
// fetches and closes the feed's TLS session, so the upload has the memory
// and the network to itself (with a fetch alongside, it could stall).
inline volatile bool g_updating = false;
struct NetBusy
{
    NetBusy()  { g_netBusy = true; }
    ~NetBusy() { g_netBusy = false; }
};

// The one long-lived TLS session (adsb.lol's, kept open between fetches)
// registers how to close it; any other TLS user that finds too little heap
// closes it first and tries again. It reopens on the next position fetch.
inline void (*g_tlsReleaser)() = nullptr;

inline bool tlsAffordable(const char *what)
{
    static constexpr uint32_t kMinFree = 75000;
    auto room = [] { return ESP.getFreeHeap() >= kMinFree && ESP.getMaxAllocHeap() >= 20000; };
    if (room()) return true;
    if (g_tlsReleaser)
    {
        Log.printf("Heap: closing the feed connection for %s\n", what);
        g_tlsReleaser();
        if (room()) return true;
    }
    static unsigned long lastLog = 0;
    if (millis() - lastLog > 60000)
    {
        lastLog = millis();
        Log.printf("Heap: %u free, deferring %s\n", (unsigned)ESP.getFreeHeap(), what);
    }
    return false;
}

inline void heapCheckpoint(const char *stage)
{
    static uint32_t low = UINT32_MAX;
    const uint32_t m = ESP.getMinFreeHeap();
    if (m >= low) return;
    if (low != UINT32_MAX)
        Log.printf("Heap: new low %u after %s (free now %u, largest block %u)\n",
                   (unsigned)m, stage, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    low = m;
}
