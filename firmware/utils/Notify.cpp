#include "utils/Notify.h"
#include "config/RuntimeConfig.h"
#include "utils/HeapWatch.h"
#include "utils/TelnetLogger.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <stdarg.h>

namespace
{
    struct Msg
    {
        uint8_t kind, priority;
        char    title[48];
        char    body[160];
        char    tags[28];
    };
    constexpr int kQueue = 3;
    Msg           s_q[kQueue];
    int           s_head = 0, s_count = 0;
    portMUX_TYPE  s_mux = portMUX_INITIALIZER_UNLOCKED;
    unsigned long s_lastMs[8] = {};   // per kind bit

    int bitOf(uint8_t kind) { for (int i = 0; i < 8; ++i) if (kind == (1u << i)) return i; return 7; }
}

void Notify::post(Kind kind, uint8_t priority, const char *tags, const char *title, const char *fmt, ...)
{
    if (!g_config.ntfy_topic[0]) return;
    if (kind != Test && !(g_config.notify_mask & kind)) return;
    const unsigned long now = millis();
    const int b = bitOf(kind);
    if (kind != Emergency && kind != Test && s_lastMs[b] && now - s_lastMs[b] < 120000UL) return;
    s_lastMs[b] = now;

    Msg m;
    m.kind = kind;
    m.priority = priority;
    strlcpy(m.title, title, sizeof(m.title));
    strlcpy(m.tags, tags ? tags : "", sizeof(m.tags));
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m.body, sizeof(m.body), fmt, ap);
    va_end(ap);

    portENTER_CRITICAL(&s_mux);
    if (s_count < kQueue)
    {
        s_q[(s_head + s_count) % kQueue] = m;
        ++s_count;
    }
    portEXIT_CRITICAL(&s_mux);
}

void Notify::loop()
{
    if (!s_count || ESP.getFreeHeap() < 50000) return;
    Msg m;
    portENTER_CRITICAL(&s_mux);
    m = s_q[s_head];
    s_head = (s_head + 1) % kQueue;
    --s_count;
    portEXIT_CRITICAL(&s_mux);

    const char *topic = g_config.ntfy_topic;
    const bool full = strncmp(topic, "http://", 7) == 0 || strncmp(topic, "https://", 8) == 0;
    const bool tls  = strncmp(topic, "https://", 8) == 0;
    if (tls && !tlsAffordable("ntfy")) return;   // dropped: better than starving the fetches
    char url[96];
    snprintf(url, sizeof(url), full ? "%s" : "http://ntfy.sh/%s", topic);

    NetBusy busy;
    WiFiClient       plain;
    WiFiClientSecure secure;
    if (tls) secure.setInsecure();
    HTTPClient http;
    if (!http.begin(tls ? static_cast<WiFiClient &>(secure) : plain, url)) return;
    http.setTimeout(5000);
    http.addHeader("Title", m.title);
    if (m.tags[0]) http.addHeader("Tags", m.tags);
    http.addHeader("Priority", String(m.priority));
    const int code = http.POST((uint8_t *)m.body, strlen(m.body));
    http.end();
    if (code == 200) Log.printf("Notify: sent \"%s\"\n", m.title);
    else             Log.printf("Notify: \"%s\" failed (HTTP %d)\n", m.title, code);
}
