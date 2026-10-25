#include "utils/Weather.h"
#include "utils/TelnetLogger.h"
#ifndef NATIVE_TEST   // the unit tests build the parser only
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#endif
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
        if (!m.station[0] && !haveWind && strlen(t) == 4 && isalpha((unsigned char)t[0]) &&
            isalpha((unsigned char)t[1]) && isalnum((unsigned char)t[2]) && isalnum((unsigned char)t[3]))
        {
            strlcpy(m.station, t, sizeof(m.station));
            continue;
        }
        if (!haveWind && !m.obsDay && strlen(t) == 7 && t[6] == 'Z' && allDigits(t, 6))
        {
            m.obsDay  = (uint8_t)((t[0] - '0') * 10 + (t[1] - '0'));
            m.obsHour = (uint8_t)((t[2] - '0') * 10 + (t[3] - '0'));
            m.obsMin  = (uint8_t)((t[4] - '0') * 10 + (t[5] - '0'));
            continue;
        }
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
        if (isWeather(t))
        {
            if (!m.wx[0]) strlcpy(m.wx, t, sizeof(m.wx));
            if (strstr(t, "TS")) (strstr(t, "VC") ? m.thunderNear : m.thunder) = true;
            continue;
        }
        // Cloud: "BKN004", "OVC012CB", "SCT030TCU"; vertical visibility "VV002", "VV///".
        if (tl >= 6 && allDigits(t + 3, 3) &&
            (!strncmp(t, "FEW", 3) || !strncmp(t, "SCT", 3) || !strncmp(t, "BKN", 3) || !strncmp(t, "OVC", 3)))
        {
            const int ft = atoi(t + 3) * 100;
            if ((t[0] == 'B' || t[0] == 'O') && (m.ceilingFt < 0 || ft < m.ceilingFt)) m.ceilingFt = ft;
            if (strstr(t + 6, "CB") || strstr(t + 6, "TCU")) m.cb = true;
            continue;
        }
        if (tl == 5 && !strncmp(t, "VV", 2))
        {
            const int ft = allDigits(t + 2, 3) ? atoi(t + 2) * 100 : 0;
            if (m.ceilingFt < 0 || ft < m.ceilingFt) m.ceilingFt = ft;
            continue;
        }
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

#ifndef NATIVE_TEST
bool Weather::fetch(Metar &out, const char *others, void (*onOther)(const Metar &), Taf *taf)
{
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    String url = String("https://aviationweather.gov/api/data/metar?ids=") + g_airport.icao;
    if (others && others[0]) { url += ","; url += others; }
    url += "&format=raw";
    if (taf) url += "&taf=true";
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
    // One report a line: ours, any others asked for, and with taf=true each
    // one's TAF after it (its later lines indented). Read as it arrives, a
    // line at a time, so the TAFs never sit in memory whole.
    WiFiClient *body = http.getStreamPtr();
    const time_t now = time(nullptr);
    bool ours = false, inTaf = false;
    char first[48] = "";
    const unsigned long t0 = millis();
    while (body && (body->connected() || body->available()) && millis() - t0 < 8000)
    {
        if (!body->available()) { delay(5); continue; }
        String line = body->readStringUntil((char)10);
        const bool continued = line.length() && line[0] == ' ';
        line.trim();
        if (!line.length()) continue;
        if (!first[0]) strlcpy(first, line.c_str(), sizeof(first));
        if (continued) { if (inTaf) tafFeed(*taf, line.c_str(), now); continue; }
        if (line.startsWith("TAF"))
        {
            // "TAF [AMD|COR] EGLL ...": only the airport's own.
            inTaf = false;
            if (taf)
            {
                const int at = (line.startsWith("TAF AMD ") || line.startsWith("TAF COR ")) ? 8 : 4;
                char st[5] = "";
                if ((int)line.length() > at) strlcpy(st, line.c_str() + at, sizeof(st));
                if (strcasecmp(st, g_airport.icao) == 0) { tafBegin(*taf); tafFeed(*taf, line.c_str(), now); inTaf = true; }
            }
            continue;
        }
        inTaf = false;
        Metar m;
        if (!parse(line.c_str(), m)) continue;
        if (!m.station[0] || strcasecmp(m.station, g_airport.icao) == 0) { out = m; ours = true; }
        else if (onOther) onOther(m);
    }
    http.end();
    if (!ours)
    {
        Log.printf("Weather: could not read \"%s\"\n", first);
        return false;
    }
    return true;
}
#endif

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

void Weather::describe(const char *wx, char *out, size_t len)
{
    if (!len) return;
    out[0] = '\0';
    if (!wx || !wx[0]) return;
    const char *intensity = wx[0] == '-' ? "light " : wx[0] == '+' ? "heavy " : "";
    const char *w = (wx[0] == '-' || wx[0] == '+') ? wx + 1 : wx;
    const char *what = nullptr;
    if (strstr(w, "TS"))                      { what = "thunder"; intensity = ""; }
    else if (strstr(w, "FZ") && strstr(w, "RA")) what = "freezing rain";
    else if (strstr(w, "SH") && strstr(w, "SN")) what = "snow showers";
    else if (strstr(w, "SH"))                 what = "showers";
    else if (strstr(w, "SN") || strstr(w, "SG")) what = "snow";
    else if (strstr(w, "RA"))                 what = "rain";
    else if (strstr(w, "DZ"))                 what = "drizzle";
    else if (strstr(w, "GR") || strstr(w, "GS") || strstr(w, "PL")) what = "hail";
    else if (strstr(w, "FG"))                 { what = "fog"; intensity = ""; }
    else if (strstr(w, "BR"))                 { what = "mist"; intensity = ""; }
    else if (strstr(w, "HZ") || strstr(w, "FU")) { what = "haze"; intensity = ""; }
    if (what) snprintf(out, len, "%s%s", intensity, what);
}

void Weather::friendly(const Metar &m, char *out, size_t len, int maxChars)
{
    if (!len) return;
    out[0] = '\0';
    if (!m.valid) return;
    char wx[20], wind[16], temp[10] = "";
    describe(m.wx, wx, sizeof(wx));
    if (!wx[0] && m.visM >= 0 && m.visM < 5000) snprintf(wx, sizeof(wx), "vis %s", m.visText);
    if (m.windKt == 0)      snprintf(wind, sizeof(wind), "calm");
    else if (m.windDir < 0) snprintf(wind, sizeof(wind), "VRB/%d", m.windKt);
    else                    snprintf(wind, sizeof(wind), "%03d/%d", m.windDir, m.windKt);
    if (m.gustKt) snprintf(wind + strlen(wind), sizeof(wind) - strlen(wind), "G%d", m.gustKt);
    if (m.tempC != -99) snprintf(temp, sizeof(temp), "%d\xC2\xB0", m.tempC);
    for (int level = 0; level < 2; ++level)
    {
        size_t o = 0;
        const char *parts[3] = {temp, wx, level == 0 ? wind : ""};
        for (const char *p : parts)
        {
            if (!p[0]) continue;
            o += snprintf(out + o, o < len ? len - o : 0, "%s%s", o ? "  " : "", p);
            if (o >= len) break;
        }
        // Characters on the panel: the degree sign is two bytes but one letter.
        if ((int)strlen(out) - (temp[0] ? 1 : 0) <= maxChars) return;
    }
}

void Weather::pressure(const Metar &m, char *out, size_t len)
{
    if (m.altInHg100)  snprintf(out, len, "A%d.%02d", m.altInHg100 / 100, m.altInHg100 % 100);
    else if (m.qnh)    snprintf(out, len, "Q%d", m.qnh);
    else if (len)      out[0] = '\0';
}

namespace { volatile int s_qnh = 0; }

void Weather::setQnh(int hPa) { s_qnh = (hPa >= 900 && hPa <= 1080) ? hPa : 0; }

double Weather::qnhAltitudeM(double pressureAltM)
{
    const int qnh = s_qnh;
    if (!qnh || isnan(pressureAltM)) return pressureAltM;
    const double ft = pressureAltM * 3.28084;
    const double full = (qnh - 1013.25) * 27.0;   // ft
    const double k = ft <= 6000 ? 1.0 : ft >= 7000 ? 0.0 : (7000 - ft) / 1000.0;
    return (ft + full * k) / 3.28084;
}

void Weather::components(const Metar &m, int runwayDeg, int &crossKt, int &tailKt)
{
    crossKt = tailKt = 0;
    if (!m.valid || m.windDir < 0) return;
    const float a = (m.windDir - runwayDeg) * (float)M_PI / 180.0f;
    crossKt = (int)lroundf(fabsf(sinf(a)) * m.windKt);
    tailKt  = (int)lroundf(-cosf(a) * m.windKt);
}

// -- The airfield's mood (8.2) ---------------------------------------------

Weather::Mood Weather::mood(const Metar &m, Mood previous)
{
    if (!m.valid) return Calm;
    if (m.thunder) return Storm;
    if (m.thunderNear) return StormNear;
    const bool low = (m.visM >= 0 && m.visM < 1000) || (m.ceilingFt >= 0 && m.ceilingFt <= 200);
    const bool still = previous == LowVis &&
                       ((m.visM >= 0 && m.visM < 1500) || (m.ceilingFt >= 0 && m.ceilingFt < 300));
    return low || still ? LowVis : Calm;
}

void Weather::moodLines(Mood k, const Metar &m, char *l1, size_t l1Len, char *l2, size_t l2Len)
{
    char place[24];
    strlcpy(place, g_airport.name, sizeof(place));
    for (char *p = place + 1; *p; ++p)   // "HEATHROW" -> "Heathrow", "SAN FRANCISCO" -> "San Francisco"
        if (p[-1] != ' ') *p = (char)tolower((unsigned char)*p);
    if (k == Storm || k == StormNear)
    {
        snprintf(l1, l1Len, "%s", k == Storm ? "Thunderstorm" : "Thunder nearby");
        snprintf(l2, l2Len, "%s %s", k == Storm ? "Over" : "Around", place);
        return;
    }
    if (k != LowVis) { if (l1Len) l1[0] = 0; if (l2Len) l2[0] = 0; return; }
    snprintf(l1, l1Len, "Low visibility");
    char what[20];
    describe(m.wx, what, sizeof(what));
    if (what[0]) what[0] = (char)toupper((unsigned char)what[0]);
    if (m.visM >= 0 && m.visM < 1500)
        snprintf(l2, l2Len, "%s %d m", what[0] ? what : "Visibility", m.visM);
    else if (m.ceilingFt >= 0)
        snprintf(l2, l2Len, "Cloud at %d ft", m.ceilingFt);
    else
        snprintf(l2, l2Len, "%s", what);
}

// -- TAF -------------------------------------------------------------------

namespace
{
    // Days since 1970-01-01 of a civil date (Howard Hinnant's algorithm);
    // a day past the month's end carries over.
    long daysFrom(int y, int m, int d)
    {
        y -= m <= 2;
        const long era = (y >= 0 ? y : y - 399) / 400;
        const unsigned yoe = (unsigned)(y - era * 400);
        const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + (long)doe - 719468;
    }
    // Day of month, hour (24 allowed) and minute, in the month around now (UTC).
    time_t tafTime(int day, int hour, int minute, time_t now)
    {
        struct tm g;
        gmtime_r(&now, &g);
        int y = g.tm_year + 1900, mo = g.tm_mon + 1;
        auto build = [&](int yy, int mm) { return (time_t)daysFrom(yy, mm, day) * 86400 + hour * 3600 + minute * 60; };
        time_t t = build(y, mo);
        if (t - now > 15L * 86400)      { if (--mo < 1)  { mo = 12; --y; } t = build(y, mo); }
        else if (now - t > 15L * 86400) { if (++mo > 12) { mo = 1;  ++y; } t = build(y, mo); }
        return t;
    }
    int two(const char *p) { return (p[0] - '0') * 10 + (p[1] - '0'); }
    bool isPeriod(const char *t) { return strlen(t) == 9 && t[4] == '/' && allDigits(t, 4) && allDigits(t + 5, 4); }
    bool space(char c) { return c == ' ' || c == 9 || c == 10 || c == 13; }
}

void Weather::tafBegin(Taf &t) { t = Taf(); }

time_t Weather::observedAt(const Metar &m, time_t now)
{
    if (!m.obsDay || now < 1600000000) return 0;
    return tafTime(m.obsDay, m.obsHour, m.obsMin, now);
}

void Weather::tafFeed(Taf &t, const char *text, time_t now)
{
    enum { Header = 0, Base, Becmg, Group, Skip };
    const char *p = text;
    char tok[24];
    while (*p)
    {
        while (*p && space(*p)) ++p;
        size_t n = 0;
        while (*p && !space(*p)) { if (n + 1 < sizeof(tok)) tok[n++] = *p; ++p; }
        tok[n] = 0;
        if (!n) continue;
        if (t.mode == Header)
        {
            if (n == 7 && tok[6] == 'Z' && allDigits(tok, 6)) t.issued = tafTime(two(tok), two(tok + 2), two(tok + 4), now);
            else if (isPeriod(tok))
            {
                t.from = tafTime(two(tok), two(tok + 2), 0, now);
                t.to   = tafTime(two(tok + 5), two(tok + 7), 0, now);
                t.pending = t.from;
                t.groupWind = false;
                t.mode = Base;
            }
            continue;
        }
        if (!strcmp(tok, "BECMG")) { t.mode = Becmg; t.groupWind = false; continue; }
        if (n == 8 && tok[0] == 'F' && tok[1] == 'M' && allDigits(tok + 2, 6))
        {
            t.pending = tafTime(two(tok + 2), two(tok + 4), two(tok + 6), now);
            t.mode = Group;
            t.groupWind = false;
            continue;
        }
        if (!strcmp(tok, "TEMPO") || !strncmp(tok, "PROB", 4) || !strcmp(tok, "INTER")) { t.mode = Skip; continue; }
        if (t.mode == Becmg && isPeriod(tok))
        {
            const time_t a = tafTime(two(tok), two(tok + 2), 0, now), b = tafTime(two(tok + 5), two(tok + 7), 0, now);
            t.pending = a + (b - a) / 2;   // a change over a few hours: count it from halfway
            t.mode = Group;
            continue;
        }
        if ((t.mode == Base || t.mode == Group) && !t.groupWind && t.n < sizeof(t.w) / sizeof(t.w[0]))
        {
            Metar w;
            if (!parseWind(tok, w)) continue;
            t.groupWind = true;
            int at = t.n;   // kept in time order
            while (at > 0 && t.w[at - 1].at > t.pending) { t.w[at] = t.w[at - 1]; --at; }
            t.w[at] = { t.pending, (int16_t)w.windDir, (uint8_t)min(w.windKt, 255), (uint8_t)min(w.gustKt, 255) };
            ++t.n;
            t.valid = t.from && t.to;
        }
    }
}

bool Weather::tafWindAt(const Taf &t, time_t when, Taf::Wind &out)
{
    if (!t.valid || !t.n || when > t.to || when < t.from - 3600) return false;
    out = t.w[0];
    for (int i = 0; i < t.n; ++i)
        if (t.w[i].at <= when) out = t.w[i];
    return true;
}

namespace
{
    float angleBetween(float a, float b) { float d = fmodf(fabsf(a - b), 360.0f); return d > 180 ? 360 - d : d; }
    // Tailwind (kt) on a landing course; variable winds count as none.
    float tailOn(float course, const Taf::Wind &w)
    {
        if (w.dir < 0) return 0;
        return -cosf((w.dir - course) * (float)M_PI / 180.0f) * w.kt;
    }
    // The airport's runway end nearest a course; nullptr if none within 30 degrees.
    const RunwayEnd *runwayNear(float course)
    {
        const RunwayEnd *best = nullptr;
        for (int i = 0; i < g_airport.runwayCount; ++i)
            if (angleBetween(g_airport.runways[i].course, course) < 30 &&
                (!best || angleBetween(g_airport.runways[i].course, course) < angleBetween(best->course, course)))
                best = &g_airport.runways[i];
        return best;
    }
}

bool Weather::runwayOutlook(const Taf &t, float currentCourse, time_t now, Outlook &out)
{
    out = Outlook();
    if (!t.valid || !t.n || isnan(currentCourse) || now > t.to) return false;
    // Heathrow prefers westerlies and takes up to 5 kt of tailwind to keep them.
    const RunwayEnd *west = g_airport.lhrAlternation ? runwayNear(270) : nullptr;
    const float preferred = west ? west->course : NAN;
    const bool onPreferred = !isnan(preferred) && angleBetween(currentCourse, preferred) < 90;
    for (int i = 0; i < t.n; ++i)
    {
        const time_t ends = i + 1 < t.n ? t.w[i + 1].at : t.to;
        if (ends <= now) continue;   // already past
        const Taf::Wind &w = t.w[i];
        const bool change = (isnan(preferred) || onPreferred) ? tailOn(currentCourse, w) >= 5
                                                              : tailOn(preferred, w) < 5;
        if (!change) continue;
        const float target = fmodf(currentCourse + 180.0f, 360.0f);
        if (const RunwayEnd *r = runwayNear(target))
        {
            strlcpy(out.to, r->name, sizeof(out.to));
            if (strlen(out.to) == 3 && isalpha((unsigned char)out.to[2])) out.to[2] = 0;   // "09L" -> "09"
        }
        else snprintf(out.to, sizeof(out.to), "%02d", ((int)lroundf(target / 10.0f) + 35) % 36 + 1);
        out.change  = true;
        out.at      = w.at > now ? w.at : now;
        out.windDir = w.dir;
        out.windKt  = w.kt;
        return true;
    }
    return true;
}
