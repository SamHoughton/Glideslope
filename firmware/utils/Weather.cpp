#include "utils/Weather.h"
#include "utils/TelnetLogger.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <math.h>
#include "config/Airport.h"

namespace
{
    bool allDigits(const char *s, int n)
    {
        for (int i = 0; i < n; ++i)
            if (!isdigit((unsigned char)s[i])) return false;
        return true;
    }

    // Wind group: dddffKT, dddffGggKT, VRBffKT (ff may be three digits).
    bool parseWind(const char *t, Metar &m)
    {
        const size_t n = strlen(t);
        if (n < 7 || strcmp(t + n - 2, "KT") != 0) return false;
        if (strncmp(t, "VRB", 3) == 0) m.windDir = -1;
        else if (allDigits(t, 3))      m.windDir = (t[0] - '0') * 100 + (t[1] - '0') * 10 + (t[2] - '0');
        else return false;
        const char *p = t + 3;
        m.windKt = atoi(p);
        const char *g = strchr(p, 'G');
        m.gustKt = g ? atoi(g + 1) : 0;
        if (m.windKt == 0) m.windDir = -1;   // calm
        return true;
    }

    // Present-weather group: optional +/-/VC, then a known descriptor or phenomenon.
    bool isWeather(const char *t)
    {
        static const char *const kCodes[] = {"DZ", "RA", "SN", "SG", "PL", "GR", "GS", "FG", "BR",
                                             "HZ", "FU", "TS", "SH", "FZ", "SQ", "DS", "SS", "UP"};
        const char *p = t;
        if (*p == '+' || *p == '-') ++p;
        if (strncmp(p, "VC", 2) == 0) p += 2;
        if (strlen(p) < 2 || strlen(p) > 6) return false;
        for (const char *c : kCodes)
            if (strncmp(p, c, 2) == 0) return true;
        return false;
    }
}

bool Weather::parse(const char *raw, Metar &m)
{
    m = Metar();
    strlcpy(m.raw, raw, sizeof(m.raw));
    char buf[sizeof(m.raw)];
    strlcpy(buf, raw, sizeof(buf));

    bool haveWind = false;
    int whole = -1;            // "1" of "1 1/2SM"
    char *save = nullptr;
    for (char *t = strtok_r(buf, " \r\n", &save); t; t = strtok_r(nullptr, " \r\n", &save))
    {
        if (!haveWind) { haveWind = parseWind(t, m); continue; }
        if (strcmp(t, "CAVOK") == 0) { m.cavok = true; m.visM = 9999; strlcpy(m.visText, "CAVOK", sizeof(m.visText)); continue; }
        if (m.visM < 0 && strlen(t) == 4 && allDigits(t, 4))
        {
            m.visM = atoi(t);
            if (m.visM >= 9999)      snprintf(m.visText, sizeof(m.visText), "10KM+");
            else if (m.visM >= 5000) snprintf(m.visText, sizeof(m.visText), "%dKM", m.visM / 1000);
            else                     snprintf(m.visText, sizeof(m.visText), "%dM", m.visM);
            continue;
        }
        // US visibility in statute miles: "10SM", "P6SM", "1/2SM", "M1/4SM", "1 1/2SM".
        const size_t tl = strlen(t);
        if (m.visM < 0 && tl > 2 && strcmp(t + tl - 2, "SM") == 0)
        {
            const char *p = t + ((t[0] == 'P' || t[0] == 'M') ? 1 : 0);
            float miles = atof(p);
            const char *slash = strchr(p, '/');
            if (slash) miles = atof(p) / max(1.0, atof(slash + 1));
            if (whole > 0) miles += whole;
            m.visM = miles >= 6 ? 9999 : (int)lroundf(miles * 1609.34f);
            if (miles == (int)miles) snprintf(m.visText, sizeof(m.visText), "%s%dSM", t[0] == 'P' ? "" : "", (int)miles);
            else                     snprintf(m.visText, sizeof(m.visText), "%.1fSM", miles);
            if (t[0] == 'P') strlcat(m.visText, "+", sizeof(m.visText));
            whole = -1;
            continue;
        }
        whole = (haveWind && m.visM < 0 && tl <= 2 && allDigits(t, tl)) ? atoi(t) : -1;
        if (!m.wx[0] && isWeather(t)) { strlcpy(m.wx, t, sizeof(m.wx)); continue; }
        // Temperature/dew point: "14/12", "M01/M03".
        const char *slash = strchr(t, '/');
        if (slash && m.tempC == -99 && (isdigit((unsigned char)t[0]) || t[0] == 'M') &&
            (slash - t == 2 || slash - t == 3))
        {
            const bool neg = t[0] == 'M';
            m.tempC = atoi(t + (neg ? 1 : 0)) * (neg ? -1 : 1);
        }
        if (t[0] == 'Q' && strlen(t) == 5 && allDigits(t + 1, 4)) m.qnh = atoi(t + 1);
        if (t[0] == 'A' && strlen(t) == 5 && allDigits(t + 1, 4) && !m.qnh)
        {
            m.altInHg100 = atoi(t + 1);
            m.qnh = (int)lroundf(m.altInHg100 / 100.0f * 33.8639f);
        }
        if (strncmp(t, "RMK", 3) == 0) break;
    }
    m.valid = haveWind;
    return m.valid;
}

bool Weather::fetch(Metar &out)
{
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    const String url = String("https://aviationweather.gov/api/data/metar?ids=") + g_airport.icao + "&format=raw";
    if (!http.begin(client, url))
        return false;
    http.setTimeout(6000);
    http.useHTTP10(true);
    http.setUserAgent("Glideslope/1.0 (+https://github.com/SamHoughton/Glideslope)");
    const int code = http.GET();
    if (code != 200)
    {
        Log.printf("Weather: HTTP %d\n", code);
        http.end();
        return false;
    }
    String body = http.getString();
    http.end();
    body.trim();
    if (!parse(body.c_str(), out))
    {
        Log.printf("Weather: could not read \"%s\"\n", body.c_str());
        return false;
    }
    return true;
}

void Weather::line(const Metar &m, char *out, size_t len, int maxChars)
{
    if (!m.valid) { if (len) out[0] = '\0'; return; }
    char wind[16], vis[8] = "", temp[8] = "";
    if (m.windKt == 0)          snprintf(wind, sizeof(wind), "CALM");
    else if (m.windDir < 0)     snprintf(wind, sizeof(wind), "VRB/%02d", m.windKt);
    else                        snprintf(wind, sizeof(wind), "%03d/%02d", m.windDir, m.windKt);
    if (m.gustKt) snprintf(wind + strlen(wind), sizeof(wind) - strlen(wind), "G%d", m.gustKt);

    strlcpy(vis, m.visText, sizeof(vis));
    if (m.tempC != -99) snprintf(temp, sizeof(temp), "%dC", m.tempC);

    // Most detail first; drop the weather group, then the temperature, to fit.
    for (int level = 0; level < 3; ++level)
    {
        snprintf(out, len, "%s%s%s%s%s%s%s", wind,
                 vis[0] ? " " : "", vis,
                 (level == 0 && m.wx[0]) ? " " : "", level == 0 ? m.wx : "",
                 (level < 2 && temp[0]) ? " " : "", level < 2 ? temp : "");
        if ((int)strlen(out) <= maxChars) return;
    }
}

void Weather::pressure(const Metar &m, char *out, size_t len)
{
    if (m.altInHg100)  snprintf(out, len, "A%d.%02d", m.altInHg100 / 100, m.altInHg100 % 100);
    else if (m.qnh)    snprintf(out, len, "Q%d", m.qnh);
    else if (len)      out[0] = '\0';
}

void Weather::components(const Metar &m, int runwayDeg, int &crossKt, int &tailKt)
{
    crossKt = tailKt = 0;
    if (!m.valid || m.windDir < 0) return;
    const float a = (m.windDir - runwayDeg) * (float)M_PI / 180.0f;
    crossKt = (int)lroundf(fabsf(sinf(a)) * m.windKt);
    tailKt  = (int)lroundf(-cosf(a) * m.windKt);
}
