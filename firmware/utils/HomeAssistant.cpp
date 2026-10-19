#include "utils/HomeAssistant.h"
#include "utils/WebConfig.h"
#include "utils/TelnetLogger.h"
#include "config/RuntimeConfig.h"
#include <WiFi.h>
#include <PubSubClient.h>

namespace HomeAssistant
{
namespace
{
// Made on first use: nothing is spent while Home Assistant is off.
WiFiClient   *g_tcp = nullptr;
PubSubClient *g_mqttPtr = nullptr;
char          g_id[13] = "";               // MAC, lower-case hex
char          g_base[40] = "";             // glideslope/<id>
unsigned long g_lastTryMs = 0, g_lastPubMs = 0;
bool          g_announced = false;
char          g_hostInUse[48] = "";

constexpr unsigned long kRetryMs   = 60000;
constexpr unsigned long kPublishMs = 30000;

struct Sensor { const char *key, *name, *icon, *unit, *deviceClass; };
const Sensor kSensors[] = {
    {"next",       "Next arrival",          "mdi:airplane-landing", nullptr, nullptr},
    {"next_eta",   "Next arrival in",       "mdi:timer-outline",    "s",     "duration"},
    {"arr_rwy",    "Arrivals runway",       "mdi:road-variant",     nullptr, nullptr},
    {"dep_rwy",    "Departures runway",     "mdi:road-variant",     nullptr, nullptr},
    {"arrivals",   "Arrivals today",        "mdi:airplane-landing", nullptr, nullptr},
    {"departures", "Departures today",      "mdi:airplane-takeoff", nullptr, nullptr},
    {"holding",    "Aircraft holding",      "mdi:rotate-right",     nullptr, nullptr},
    {"last",       "Last flight",           "mdi:airplane",         nullptr, nullptr},
    {"spotted",    "Last spotted overhead", "mdi:binoculars",       nullptr, nullptr},
};

// JSON string value: quotes and backslashes escaped, control characters dropped.
void jsonStr(char *&p, char *end, const char *s)
{
    if (p < end) *p++ = '"';
    for (; *s && p + 2 < end; ++s)
    {
        if (*s == '"' || *s == '\\') *p++ = '\\';
        if ((unsigned char)*s >= 0x20) *p++ = *s;
    }
    if (p < end) *p++ = '"';
    *p = '\0';
}

void announce()
{
    char topic[96], payload[480];
    for (const Sensor &s : kSensors)
    {
        snprintf(topic, sizeof(topic), "homeassistant/sensor/glideslope_%s/%s/config", g_id, s.key);
        int n = snprintf(payload, sizeof(payload),
                         "{\"name\":\"%s\",\"uniq_id\":\"glideslope_%s_%s\",\"stat_t\":\"%s/state\","
                         "\"val_tpl\":\"{{ value_json.%s }}\",\"avty_t\":\"%s/status\",\"ic\":\"%s\"",
                         s.name, g_id, s.key, g_base, s.key, g_base, s.icon);
        if (s.unit) n += snprintf(payload + n, sizeof(payload) - n, ",\"unit_of_meas\":\"%s\"", s.unit);
        if (s.deviceClass) n += snprintf(payload + n, sizeof(payload) - n, ",\"dev_cla\":\"%s\"", s.deviceClass);
        snprintf(payload + n, sizeof(payload) - n,
                 ",\"dev\":{\"ids\":[\"glideslope_%s\"],\"name\":\"Glideslope\",\"mf\":\"Glideslope\","
                 "\"mdl\":\"HD-WF2\",\"sw\":\"%s\",\"cu\":\"http://%s/\"}}",
                 g_id, GLIDESLOPE_VERSION, WiFi.localIP().toString().c_str());
        g_mqttPtr->publish(topic, payload, true);
    }
    g_announced = true;
}

void publish(const State &s)
{
    char buf[400], topic[56];
    char *p = buf, *end = buf + sizeof(buf) - 1;
    p += snprintf(p, end - p, "{\"next\":");
    jsonStr(p, end, s.next[0] ? s.next : "none");
    if (s.nextEtaSec >= 0) p += snprintf(p, end - p, ",\"next_eta\":%d", s.nextEtaSec);
    else                   p += snprintf(p, end - p, ",\"next_eta\":null");
    p += snprintf(p, end - p, ",\"arr_rwy\":");
    jsonStr(p, end, s.arrRunways[0] ? s.arrRunways : "none");
    p += snprintf(p, end - p, ",\"dep_rwy\":");
    jsonStr(p, end, s.depRunways[0] ? s.depRunways : "none");
    p += snprintf(p, end - p, ",\"arrivals\":%d,\"departures\":%d,\"holding\":%d,\"last\":",
                  s.arrivals, s.departures, s.holding);
    jsonStr(p, end, s.last[0] ? s.last : "none");
    p += snprintf(p, end - p, ",\"spotted\":");
    jsonStr(p, end, s.spotted[0] ? s.spotted : "none");
    if (p < end) *p++ = '}';
    *p = '\0';
    snprintf(topic, sizeof(topic), "%s/state", g_base);
    g_mqttPtr->publish(topic, buf, false);
}
}

void loop(const State &s)
{
    // Off, or the broker changed: drop any connection.
    if (!g_config.mqtt_host[0] || strcmp(g_hostInUse, g_config.mqtt_host) != 0)
    {
        if (g_mqttPtr && g_mqttPtr->connected()) g_mqttPtr->disconnect();
        g_announced = false;
        strlcpy(g_hostInUse, g_config.mqtt_host, sizeof(g_hostInUse));
        if (!g_config.mqtt_host[0]) return;
    }
    if (WiFi.status() != WL_CONNECTED) return;

    if (!g_mqttPtr)
    {
        g_tcp = new WiFiClient();
        g_mqttPtr = new PubSubClient(*g_tcp);
    }
    if (!g_id[0])
    {
        uint8_t mac[6];
        WiFi.macAddress(mac);
        snprintf(g_id, sizeof(g_id), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        snprintf(g_base, sizeof(g_base), "glideslope/%s", g_id);
        g_mqttPtr->setBufferSize(512);
        g_mqttPtr->setSocketTimeout(3);
        g_mqttPtr->setKeepAlive(60);
    }

    const unsigned long now = millis();
    if (!g_mqttPtr->connected())
    {
        g_announced = false;
        if (g_lastTryMs && now - g_lastTryMs < kRetryMs) return;
        g_lastTryMs = now;
        if (ESP.getFreeHeap() < 30000) return;   // the feed's TLS comes first
        g_mqttPtr->setServer(g_hostInUse, g_config.mqtt_port ? g_config.mqtt_port : 1883);
        char client[24], will[56];
        snprintf(client, sizeof(client), "glideslope-%s", g_id + 6);
        snprintf(will, sizeof(will), "%s/status", g_base);
        const bool ok = g_config.mqtt_user[0]
            ? g_mqttPtr->connect(client, g_config.mqtt_user, g_config.mqtt_pass, will, 0, true, "offline")
            : g_mqttPtr->connect(client, will, 0, true, "offline");
        if (!ok)
        {
            Log.printf("Home Assistant: could not connect to %s (state %d)\n", g_hostInUse, g_mqttPtr->state());
            return;
        }
        Log.printf("Home Assistant: connected to %s\n", g_hostInUse);
        g_mqttPtr->publish(will, "online", true);
        g_lastPubMs = 0;
    }
    g_mqttPtr->loop();
    if (!g_announced) announce();
    if (!g_lastPubMs || now - g_lastPubMs >= kPublishMs)
    {
        g_lastPubMs = now;
        publish(s);
    }
}
}
