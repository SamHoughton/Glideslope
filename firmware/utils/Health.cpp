#include "utils/Health.h"
#include "utils/StageTrace.h"
#include "utils/TlsArena.h"
#include "utils/Logbook.h"
#include "utils/WebConfig.h"   // GLIDESLOPE_VERSION
#include <LittleFS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_system.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <WiFiClient.h>
#include <time.h>
#include "utils/TelnetLogger.h"

namespace
{
    // A chosen restart's reason, kept through the restart (RTC memory isn't
    // cleared by a software reset; the magic says it was set on purpose).
    // The marker is the reason's own checksum: firmware without this (8.1)
    // reuses the same RTC memory, and the text may not survive it.
    constexpr uint32_t   kMagic = 0x48454C54;   // "HELT"
    uint32_t whySum(const char *s, size_t n)
    {
        uint32_t h = kMagic;
        for (size_t i = 0; i < n; ++i) h = (h ^ (uint8_t)s[i]) * 16777619u;
        return h;
    }
    RTC_NOINIT_ATTR uint32_t s_whyMagic;
    RTC_NOINIT_ATTR char     s_why[40];

    char s_boot[40] = "unknown";   // why this boot happened, short
    bool s_bootPending = true;     // to be logged once the clock is set

    // 24 hours in 15-minute slots: the lowest free heap and the smallest
    // largest-block in each (KB), oldest first from s_head.
    constexpr int     kSlots = 96;
    constexpr unsigned long kSlotMs = 15UL * 60 * 1000;
    uint8_t           s_free[kSlots], s_block[kSlots];
    uint8_t           s_head = 0, s_count = 0;
    uint32_t          s_winFree = UINT32_MAX, s_winBlock = UINT32_MAX;
    unsigned long     s_winStart = 0, s_lastSampleMs = 0;

    uint32_t      s_fetchOk = 0, s_fetchFail = 0, s_wxOk = 0, s_wxFail = 0;
    unsigned long s_lastFetchOkMs = 0, s_lastWxOkMs = 0;

    struct Boot { uint32_t at; char why[28]; };   // Unix time, reason
    constexpr int kBoots = 8;

    uint8_t kb(uint32_t bytes) { return (uint8_t)min<uint32_t>(255, (bytes + 512) / 1024); }

    void addJsonString(String &out, const char *s)
    {
        out += '"';
        for (; *s; ++s)
        {
            if (*s == '"' || *s == '\\') out += '\\';
            if ((uint8_t)*s >= 32) out += *s;
        }
        out += '"';
    }
    long agoS(unsigned long ms) { return ms ? (long)((millis() - ms) / 1000) : -1; }
}

void Health::begin()
{
    const esp_reset_reason_t r = esp_reset_reason();
    if (r == ESP_RST_SW && s_whyMagic == whySum(s_why, sizeof(s_why)) && s_why[0])
    {
        s_why[sizeof(s_why) - 1] = 0;
        strlcpy(s_boot, s_why, sizeof(s_boot));
    }
    else
        strlcpy(s_boot, r == ESP_RST_POWERON  ? "power on"
                      : r == ESP_RST_SW       ? "restart"
                      : r == ESP_RST_TASK_WDT ? "watchdog: a task hung"
                      : r == ESP_RST_INT_WDT  ? "watchdog: interrupts"
                      : r == ESP_RST_WDT      ? "watchdog"
                      : r == ESP_RST_PANIC    ? "crash"
                      : r == ESP_RST_BROWNOUT ? "brownout: a power dip"
                      : r == ESP_RST_EXT      ? "reset button"
                                              : "other (e.g. flashing)", sizeof(s_boot));
    s_whyMagic = 0;
    s_winStart = millis();
}

void Health::noteRestart(const char *why)
{
    memset(s_why, 0, sizeof(s_why));
    strlcpy(s_why, why, sizeof(s_why));
    s_whyMagic = whySum(s_why, sizeof(s_why));
}

void Health::saveBlackBox(const char *why)
{
    File f = LittleFS.open("/log/blackbox.txt", "w");
    if (!f) return;
    multi_heap_info_t hi;
    heap_caps_get_info(&hi, MALLOC_CAP_8BIT);
    char when[24] = "clock not set";
    const time_t now = time(nullptr);
    struct tm lt;
    if (now > 1600000000) { localtime_r(&now, &lt); strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &lt); }
    f.printf("%s  %s  (%s, up %lus)\n", when, why, GLIDESLOPE_VERSION, (unsigned long)(millis() / 1000));
    f.printf("heap free %u, largest block %u, lowest %u, free blocks %u, allocated blocks %u; TLS slots free %d\n\n",
             (unsigned)hi.total_free_bytes, (unsigned)hi.largest_free_block, (unsigned)hi.minimum_free_bytes,
             (unsigned)hi.free_blocks, (unsigned)hi.allocated_blocks, (int)TlsArena::slotsFree());
    char st[96];
    StageTrace::describe(StageTrace::Web, st, sizeof(st));
    f.printf("web task: %s\n", st);
    StageTrace::describe(StageTrace::Main, st, sizeof(st));
    f.printf("main loop: %s\n\n", st);
    Log.writeLines(f);
    f.close();
}

void Health::writeBlackBox(WiFiClient &c)
{
    File f = LittleFS.open("/log/blackbox.txt", "r");
    if (!f)
    {
        c.print("HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nNo self-heal restart yet.\n");
        return;
    }
    c.printf("HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: %u\r\nConnection: close\r\n\r\n",
             (unsigned)f.size());
    uint8_t buf[256];
    size_t n;
    while ((n = f.read(buf, sizeof(buf))) > 0) c.write(buf, n);
    f.close();
}

void Health::noteFetch(bool ok)
{
    if (ok) { ++s_fetchOk; s_lastFetchOkMs = millis(); }
    else ++s_fetchFail;
}

void Health::noteWeather(bool ok)
{
    if (ok) { ++s_wxOk; s_lastWxOkMs = millis(); }
    else ++s_wxFail;
}

void Health::loop()
{
    const unsigned long now = millis();
    if (now - s_lastSampleMs >= 60000UL || !s_lastSampleMs)
    {
        s_lastSampleMs = now;
        s_winFree  = min<uint32_t>(s_winFree, ESP.getFreeHeap());
        s_winBlock = min<uint32_t>(s_winBlock, ESP.getMaxAllocHeap());
        if (now - s_winStart >= kSlotMs)
        {
            const int at = (s_head + s_count) % kSlots;
            s_free[at] = kb(s_winFree);
            s_block[at] = kb(s_winBlock);
            if (s_count < kSlots) ++s_count;
            else s_head = (s_head + 1) % kSlots;
            s_winFree = s_winBlock = UINT32_MAX;
            s_winStart = now;
        }
    }
    // The boot, with when it happened, once the clock knows.
    const time_t t = time(nullptr);
    if (s_bootPending && t > 1600000000)
    {
        s_bootPending = false;
        Boot boots[kBoots] = {};
        Preferences p;
        if (p.begin("health", false))
        {
            p.getBytes("boots", boots, sizeof(boots));
            memmove(&boots[1], &boots[0], sizeof(Boot) * (kBoots - 1));
            boots[0].at = (uint32_t)(t - now / 1000);
            strlcpy(boots[0].why, s_boot, sizeof(boots[0].why));
            p.putBytes("boots", boots, sizeof(boots));
            p.putUInt("count", p.getUInt("count", 0) + 1);
            p.end();
        }
    }
}

void Health::json(String &out)
{
    out = "";
    out.reserve(1800);
    char b[96];
    snprintf(b, sizeof(b), "{\"version\":\"%s\",\"built\":\"%s\",\"uptime_s\":%lu,\"boot\":", GLIDESLOPE_VERSION,
             __DATE__ " " __TIME__, millis() / 1000);
    out += b;
    addJsonString(out, s_boot);
    out += ",\"last_reset\":";
    addJsonString(out, StageTrace::lastReset());

    // The restarts before, newest first.
    Boot boots[kBoots] = {};
    uint32_t count = 0;
    Preferences p;
    if (p.begin("health", true))
    {
        p.getBytes("boots", boots, sizeof(boots));
        count = p.getUInt("count", 0);
        p.end();
    }
    snprintf(b, sizeof(b), ",\"boot_count\":%u,\"boots\":[", (unsigned)count);
    out += b;
    bool first = true;
    for (const Boot &x : boots)
    {
        if (!x.at) continue;
        char why[sizeof(x.why) + 1];
        strlcpy(why, x.why, sizeof(why));
        snprintf(b, sizeof(b), "%s{\"at\":%u,\"why\":", first ? "" : ",", (unsigned)x.at);
        out += b;
        addJsonString(out, why);
        out += '}';
        first = false;
    }

    // Memory: now, and the last 24 hours (the slot in progress last).
    snprintf(b, sizeof(b), "],\"heap\":{\"free\":%u,\"block\":%u,\"min_free\":%u,\"every_min\":15,\"free_kb\":[",
             (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap(), (unsigned)ESP.getMinFreeHeap());
    out += b;
    for (int pass = 0; pass < 2; ++pass)
    {
        if (pass) out += "],\"block_kb\":[";
        const uint8_t *v = pass ? s_block : s_free;
        for (int i = 0; i < s_count; ++i)
        {
            snprintf(b, sizeof(b), "%s%u", i ? "," : "", (unsigned)v[(s_head + i) % kSlots]);
            out += b;
        }
        const uint32_t now = pass ? min<uint32_t>(s_winBlock, ESP.getMaxAllocHeap()) : min<uint32_t>(s_winFree, ESP.getFreeHeap());
        snprintf(b, sizeof(b), "%s%u", s_count ? "," : "", (unsigned)kb(now));
        out += b;
    }
    snprintf(b, sizeof(b), "]},\"feed\":{\"ok\":%u,\"failed\":%u,\"last_ok_s\":%ld}",
             (unsigned)s_fetchOk, (unsigned)s_fetchFail, agoS(s_lastFetchOkMs));
    out += b;
    snprintf(b, sizeof(b), ",\"weather\":{\"ok\":%u,\"failed\":%u,\"last_ok_s\":%ld}",
             (unsigned)s_wxOk, (unsigned)s_wxFail, agoS(s_lastWxOkMs));
    out += b;
    snprintf(b, sizeof(b), ",\"wifi\":{\"rssi\":%d,\"channel\":%d}", (int)WiFi.RSSI(), (int)WiFi.channel());
    out += b;
    snprintf(b, sizeof(b), ",\"storage\":{\"used\":%u,\"total\":%u,\"logbook\":%u}", (unsigned)LittleFS.usedBytes(),
             (unsigned)LittleFS.totalBytes(), (unsigned)Logbook::bytesUsed());
    out += b;
    snprintf(b, sizeof(b), ",\"temp_c\":%.1f,\"tls\":{\"served\":%u,\"to_heap\":%u,\"largest_small\":%u}}", temperatureRead(),
             (unsigned)TlsArena::served(), (unsigned)TlsArena::overflowed(), (unsigned)TlsArena::largestSmall());
    out += b;
}
