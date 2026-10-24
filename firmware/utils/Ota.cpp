#include "utils/Ota.h"
#include "utils/HeapWatch.h"
#include "utils/ImageCheck.h"
#include "utils/Health.h"
#include "utils/TelnetLogger.h"
#include "adapters/NeoMatrixDisplay.h"   // requestPanelMessage
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>
#include <esp_task_wdt.h>
#include <esp_attr.h>
#include <esp_system.h>

namespace
{
    portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
    char         s_wanted[16] = "";
    char         s_error[64] = "";
    // The release to install after the restart (RTC memory survives a
    // software reset), and its checksum so stale bytes are never taken for one.
    RTC_NOINIT_ATTR char     s_bootTag[16];
    RTC_NOINIT_ATTR uint32_t s_bootSum;
    uint32_t tagSum(const char *t)
    {
        uint32_t h = 0x4F544131;   // "OTA1"
        for (size_t i = 0; i < 16; ++i) h = (h ^ (uint8_t)t[i]) * 16777619u;
        return h;
    }
}

void Ota::restartToInstall(const char *tag)
{
    memset(s_bootTag, 0, sizeof(s_bootTag));
    strlcpy(s_bootTag, tag, sizeof(s_bootTag));
    s_bootSum = tagSum(s_bootTag);
    Log.printf("Update: restarting to install %s\n", tag);
    requestPanelMessage("UPDATING", 20000);
    char why[32];
    snprintf(why, sizeof(why), "to install %s", tag);
    Health::noteRestart(why);
    delay(300);
    ESP.restart();
}

bool Ota::takeBootRequest(char *tag, size_t len)
{
    const bool have = esp_reset_reason() == ESP_RST_SW && s_bootSum == tagSum(s_bootTag) && s_bootTag[0] == 'v';
    if (have) strlcpy(tag, s_bootTag, len);
    s_bootSum = 0;
    return have;
}

Ota::Writer &Ota::writer()
{
    static Writer w;   // FirmwareSig's ~200 bytes: not on a task's stack
    return w;
}

const char *Ota::Writer::begin(size_t length)
{
    _length = length;
    _got = 0;
    if (!length) return "empty upload";
    if (length > ESP.getFreeSketchSpace()) return "file too large for the app slot";
    if (!Update.begin(length, U_FLASH)) return Update.errorString();
    _sig.begin(length);
    return nullptr;
}

const char *Ota::Writer::write(const uint8_t *buf, size_t n)
{
    if (_got == 0)
    {
        // ESP32 image: magic 0xE9, chip id at byte 12 (9 = ESP32-S3), and an
        // application (not a bootloader or merged image): the app
        // description's magic word 0xABCD5432 at byte 32.
        if (n < 36 || buf[0] != 0xE9) return "not a firmware image";
        const uint16_t chip = buf[12] | (buf[13] << 8);
        const uint32_t appMagic = buf[32] | (buf[33] << 8) | (buf[34] << 16) | ((uint32_t)buf[35] << 24);
        if (chip != 9) return "firmware is for a different chip";
        if (appMagic != 0xABCD5432) return "not an app image (use firmware.bin, not the full image)";
    }
    _sig.update(buf, n);
    if (Update.write(const_cast<uint8_t *>(buf), n) != n) return Update.errorString();
    _got += n;
    return nullptr;
}

const char *Ota::Writer::finish(bool allowUnsigned, bool &signedRelease)
{
    if (_got != _length) { Update.abort(); return "upload interrupted"; }
    const FirmwareSig::Result verdict = _sig.finish();
    signedRelease = verdict == FirmwareSig::Signed;
    if (verdict == FirmwareSig::Bad) { Update.abort(); return "signature does not match: not a genuine release"; }
    if (verdict == FirmwareSig::Unsigned && !allowUnsigned) { Update.abort(); return "unsigned"; }
    if (!Update.end(true)) return Update.errorString();
    return nullptr;
}

void Ota::Writer::abort() { Update.abort(); }

bool Ota::request(const char *tag)
{
    // "v8.2.0": a v, digits and dots, nothing a URL could be bent with.
    const size_t n = tag ? strlen(tag) : 0;
    if (n < 2 || n >= sizeof(s_wanted) || tag[0] != 'v') return false;
    for (size_t i = 1; i < n; ++i)
        if (!isdigit((unsigned char)tag[i]) && tag[i] != '.') return false;
    portENTER_CRITICAL(&s_mux);
    strlcpy(s_wanted, tag, sizeof(s_wanted));
    s_error[0] = 0;
    portEXIT_CRITICAL(&s_mux);
    return true;
}

bool Ota::takeRequest(char *tag, size_t len)
{
    portENTER_CRITICAL(&s_mux);
    const bool have = s_wanted[0] != 0;
    if (have) { strlcpy(tag, s_wanted, len); s_wanted[0] = 0; }
    portEXIT_CRITICAL(&s_mux);
    return have;
}

const char *Ota::lastError() { return s_error; }

const char *Ota::installRelease(const char *tag)
{
    auto fail = [&](const char *why) {
        Log.printf("Update: %s failed: %s\n", tag, why);
        strlcpy(s_error, why, sizeof(s_error));
        requestPanelMessage("UPDATE FAILED", 6000);
        return s_error;
    };
    Log.printf("Update: fetching %s from GitHub (heap %u, block %u)\n", tag, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    requestPanelMessage("DOWNLOADING", 20000);
    struct Updating { Updating() { g_updating = true; } ~Updating() { g_updating = false; } } updating;
    // The feed's connection closed first: the download wants its memory.
    if (g_tlsReleaser) g_tlsReleaser();
    NetBusy busy;   // and the web server waits (the page polls until the board is back)

    char url[112];
    snprintf(url, sizeof(url), "https://github.com/%s/releases/download/%s/firmware.bin", kRepo, tag);
    WiFiClientSecure client;
    client.setInsecure();   // the image's signature is the check that matters
    HTTPClient http;
    if (!http.begin(client, url)) return fail("could not start the download");
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);   // to GitHub's file store
    http.setTimeout(15000);
    http.useHTTP10(true);
    http.setUserAgent("Glideslope/1.0 (+https://github.com/SamHoughton/Glideslope)");
    esp_task_wdt_reset();
    const int code = http.GET();
    esp_task_wdt_reset();
    if (code != 200)
    {
        Log.printf("Update: GET -> %d (%s), heap %u blk %u, location %.60s\n", code, http.errorToString(code).c_str(),
                   (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap(), http.getLocation().c_str());
        char tlsErr[64] = "";
        if (client.lastError(tlsErr, sizeof(tlsErr))) Log.printf("Update: TLS: %s\n", tlsErr);
        http.end();
        static char why[40];
        snprintf(why, sizeof(why), "download failed (HTTP %d)", code);
        return fail(why);
    }
    const int length = http.getSize();
    if (length <= 0) { http.end(); return fail("download has no length"); }

    Writer &w = writer();
    if (const char *e = w.begin(length)) { http.end(); return fail(e); }
    WiFiClient *s = http.getStreamPtr();
    uint8_t buf[1024];
    int lastTenth = -1;
    const unsigned long t0 = millis();
    unsigned long lastData = t0;
    while ((int)w.written() < length)
    {
        esp_task_wdt_reset();
        const int avail = s->available();
        // The first read must hold the image header (36 bytes) for the check.
        if (avail <= 0 || (w.written() == 0 && avail < 36))
        {
            // Patient: with memory this tight a dropped packet can cost TCP
            // tens of seconds of backing off before it is sent again.
            if (millis() - lastData > 45000 || (avail <= 0 && !s->connected())) break;
            delay(2);
            continue;
        }
        const size_t want = min((size_t)sizeof(buf), min((size_t)avail, (size_t)length - w.written()));
        const int n = s->read(buf, want);
        if (n <= 0) continue;
        lastData = millis();
        if (const char *e = w.write(buf, n)) { w.abort(); http.end(); return fail(e); }
        const int tenth = (int)((int64_t)w.written() * 10 / length);
        if (tenth != lastTenth)
        {
            lastTenth = tenth;
            char msg[20];
            snprintf(msg, sizeof(msg), "UPDATING %d%%", tenth * 10);
            requestPanelMessage(msg, 20000);
        }
    }
    const bool closed = !s->connected();
    http.end();
    Log.printf("Update: %u of %d bytes in %lu s%s\n", (unsigned)w.written(), length, (millis() - t0) / 1000,
               (int)w.written() < length ? (closed ? ", connection closed" : ", stalled") : "");
    if ((int)w.written() < length)
    {
        w.abort();
        static char why[48];
        snprintf(why, sizeof(why), "download stopped at %u of %d KB", (unsigned)(w.written() / 1024), length / 1024);
        return fail(why);
    }
    bool signedRelease = false;
    if (const char *e = w.finish(false, signedRelease)) return fail(e);

    Log.printf("Update: %s installed from GitHub, restarting\n", tag);
    requestPanelMessage("RESTARTING");
    delay(500);
    ImageCheck::keep("restart requested");
    char why[32];
    snprintf(why, sizeof(why), "update to %s (GitHub)", tag);
    Health::noteRestart(why);
    ESP.restart();
    return nullptr;
}
