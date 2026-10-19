#include "utils/Spotter.h"
#include "utils/GeoUtils.h"
#include "display/ApproachModel.h"
#include <math.h>

namespace Spotter
{
namespace
{
constexpr unsigned long kWatchCooldownMs    = 6UL * 60 * 60 * 1000;
constexpr unsigned long kOverheadCooldownMs = 45UL * 60 * 1000;

// Recently announced aircraft: a small fixed table, oldest replaced.
struct Recent { uint32_t key; unsigned long ms; };
Recent g_recent[16] = {};

uint32_t keyOf(const StateVector &s, bool watch)
{
    uint32_t k = (uint32_t)strtoul(s.icao24.c_str(), nullptr, 16);
    if (!k)   // no address: the call sign
        for (size_t i = 0; i < s.callsign.length(); ++i) k = k * 31 + (uint8_t)s.callsign[i];
    return (k << 1 | (watch ? 1u : 0u)) | 0x80000000u;
}

bool coolingDown(uint32_t key, unsigned long now, unsigned long cooldown)
{
    for (const Recent &r : g_recent)
        if (r.key == key && now - r.ms < cooldown) return true;
    return false;
}

void remember(uint32_t key, unsigned long now)
{
    Recent *slot = &g_recent[0];
    for (Recent &r : g_recent)
    {
        if (r.key == key) { slot = &r; break; }
        if (now - r.ms > now - slot->ms || !r.key) slot = &r;
    }
    slot->key = key;
    slot->ms = now;
}

// Upper case without dashes or spaces, at most len - 1 characters.
void norm(const char *in, char *out, size_t len)
{
    size_t n = 0;
    for (; *in && n + 1 < len; ++in)
        if (*in != '-' && *in != ' ') out[n++] = (char)toupper((unsigned char)*in);
    out[n] = '\0';
}

bool matches(const char *entry, const String &field)
{
    if (!field.length()) return false;
    char f[16];
    norm(field.c_str(), f, sizeof(f));
    const size_t n = strlen(entry);
    if (n && entry[n - 1] == '*') return n > 1 && strncmp(f, entry, n - 1) == 0;
    return strcmp(f, entry) == 0;
}
}

void reset() { memset(g_recent, 0, sizeof(g_recent)); }

bool watchMatch(const char *list, const StateVector &s, char *matched, size_t len)
{
    if (!list) return false;
    const char *p = list;
    while (*p)
    {
        while (*p == ',' || *p == ' ' || *p == ';' || *p == '\n') ++p;
        char raw[16];
        size_t n = 0;
        while (*p && *p != ',' && *p != ' ' && *p != ';' && *p != '\n')
        {
            if (n + 1 < sizeof(raw)) raw[n++] = *p;
            ++p;
        }
        raw[n] = '\0';
        if (!n) continue;
        char e[16];
        norm(raw, e, sizeof(e));
        if (!e[0]) continue;
        if (matches(e, s.registration) || matches(e, s.callsign) || matches(e, s.aircraft_type) ||
            matches(e, s.icao24))
        {
            if (matched && len) strlcpy(matched, raw, len);
            return true;
        }
    }
    return false;
}

bool check(const std::vector<StateVector> &states, const Settings &cfg, unsigned long nowMs, Event &out)
{
    out = Event{};
    // The watchlist first.
    if (cfg.watchlist && cfg.watchlist[0])
        for (size_t i = 0; i < states.size(); ++i)
        {
            char m[16];
            if (!watchMatch(cfg.watchlist, states[i], m, sizeof(m))) continue;
            const uint32_t key = keyOf(states[i], true);
            if (coolingDown(key, nowMs, kWatchCooldownMs)) continue;
            remember(key, nowMs);
            out.index = (int)i;
            out.watch = true;
            strlcpy(out.matched, m, sizeof(out.matched));
            if (cfg.home_lat || cfg.home_lon)
                out.distKm = (float)haversineKm(cfg.home_lat, cfg.home_lon, states[i].lat, states[i].lon);
            return true;
        }

    // Then the closest aircraft overhead.
    if (cfg.overhead_km <= 0 || (!cfg.home_lat && !cfg.home_lon)) return false;
    int best = -1;
    float bestKm = cfg.overhead_km;
    for (size_t i = 0; i < states.size(); ++i)
    {
        const StateVector &s = states[i];
        if (s.on_ground || isnan(s.lat) || isnan(s.lon) || isnan(s.baro_altitude) || s.baro_altitude < 60) continue;
        const float km = (float)haversineKm(cfg.home_lat, cfg.home_lon, s.lat, s.lon);
        if (km > bestKm) continue;
        if (!cfg.overhead_all)
        {
            // Arriving or departing here: it has a card already.
            FlightInfo f;
            f.lat = s.lat; f.lon = s.lon; f.heading = s.heading;
            f.baro_altitude = s.baro_altitude * 3.28084;
            f.velocity = isnan(s.velocity) ? NAN : s.velocity * 1.94384;
            f.vertical_rate = isnan(s.vertical_rate) ? NAN : s.vertical_rate * 196.85;
            const ApproachStatus st = ApproachModel::evaluate(f);
            if (st.phase == ApproachStatus::Approach || st.phase == ApproachStatus::Landing ||
                st.phase == ApproachStatus::Inbound) continue;
            if (ApproachModel::climbingOut(f, nullptr, 0)) continue;
        }
        if (coolingDown(keyOf(s, false), nowMs, kOverheadCooldownMs)) continue;
        best = (int)i;
        bestKm = km;
    }
    if (best < 0) return false;
    remember(keyOf(states[best], false), nowMs);
    out.index = best;
    out.distKm = bestKm;
    return true;
}
}
