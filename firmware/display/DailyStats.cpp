#include "display/DailyStats.h"
#include "display/ApproachModel.h"
#include "config/AirlineCodes.h"
#include "utils/TelnetLogger.h"
#include <LittleFS.h>
#include <time.h>

namespace
{
    constexpr const char *kPath = "/stats.bin";
    constexpr uint32_t    kMagic = 0x31535347;   // "GSS1"

    bool localNow(struct tm &lt)
    {
        const time_t now = time(nullptr);
        if (now < 1600000000) return false;
        localtime_r(&now, &lt);
        return true;
    }

    uint32_t fnv(const String &a, const String &b, char dir)
    {
        uint32_t h = 2166136261u;
        for (size_t i = 0; i < a.length(); ++i) { h ^= (uint8_t)a[i]; h *= 16777619u; }
        h ^= '|'; h *= 16777619u;
        for (size_t i = 0; i < b.length(); ++i) { h ^= (uint8_t)b[i]; h *= 16777619u; }
        h ^= (uint8_t)dir; h *= 16777619u;
        return h ? h : 1;   // 0 marks an empty slot
    }

    // "BAW123" -> "BA" (IATA), else the ICAO prefix; empty for registrations.
    String airlineOf(const String &callsign)
    {
        if (callsign.length() < 4) return String();
        for (int i = 0; i < 3; ++i)
            if (!isalpha((unsigned char)callsign[i])) return String();
        if (!isdigit((unsigned char)callsign[3])) return String();
        String icao = callsign.substring(0, 3);
        icao.toUpperCase();
        const char *iata = airlineIata(icao.c_str());
        return iata ? String(iata) : icao;
    }

    template <typename T> void put(std::vector<uint8_t> &v, const T &x)
    {
        const uint8_t *p = (const uint8_t *)&x;
        v.insert(v.end(), p, p + sizeof(T));
    }
    template <typename T> bool get(File &f, T &x) { return f.read((uint8_t *)&x, sizeof(T)) == sizeof(T); }
}

void DailyStats::rollDay()
{
    struct tm lt;
    if (!localNow(lt)) return;   // clock not synced yet: keep counting
    if (lt.tm_yday == _day && lt.tm_year == _year) return;
    if (_day >= 0)
        Log.printf("Stats: new day (yesterday %u arrivals, %u departures)\n", _arrivals, _departures);
    reset();
    _year = lt.tm_year;
    _day  = lt.tm_yday;
    _dirty = true;
}

void DailyStats::reset()
{
    _year = _day = -1;
    _arrivals = _departures = _goArounds = 0;
    memset(_arrHour, 0, sizeof(_arrHour));
    memset(_depHour, 0, sizeof(_depHour));
    memset(_seen, 0, sizeof(_seen));
    _seenCount = 0;
    _airlines.clear();
    _types.clear();
    _seq = 0;
}

bool DailyStats::wasSeen(uint32_t h) const
{
    for (size_t i = h % kSeenSlots;; i = (i + 1) % kSeenSlots)
    {
        if (_seen[i] == h) return true;
        if (_seen[i] == 0) return false;
    }
}

bool DailyStats::markSeen(uint32_t h)
{
    if (_seenCount >= kSeenSlots * 3 / 4) return false;   // full for today: stop counting
    for (size_t i = h % kSeenSlots;; i = (i + 1) % kSeenSlots)
    {
        if (_seen[i] == h) return false;
        if (_seen[i] == 0) { _seen[i] = h; ++_seenCount; return true; }
    }
}

void DailyStats::noteTraffic(const std::vector<StateVector> &states)
{
    rollDay();
    struct tm lt;
    const int hour = localNow(lt) ? lt.tm_hour : -1;
    for (const StateVector &s : states)
    {
        FlightInfo f;   // just enough for the approach model (no route)
        f.lat = s.lat;  f.lon = s.lon;  f.heading = s.heading;
        f.baro_altitude = isnan(s.baro_altitude) ? NAN : s.baro_altitude * 3.28084;
        f.velocity      = isnan(s.velocity) ? NAN : s.velocity * 1.94384;
        f.vertical_rate = isnan(s.vertical_rate) ? NAN : s.vertical_rate * 196.85;
        if (isnan(f.baro_altitude) || f.baro_altitude > 5000) continue;

        const ApproachStatus st = ApproachModel::evaluate(f);
        const bool onFinal = (st.phase == ApproachStatus::Approach || st.phase == ApproachStatus::Landing) &&
                             !isnan(st.distKm) && st.distKm < 12.0f;
        if (onFinal)
        {
            if (markSeen(fnv(s.icao24, s.callsign, 'A'))) count(s, true, hour);
            continue;
        }
        // Climbing out along a runway; not an arrival going around.
        if (ApproachModel::climbingOut(f, nullptr, 0) && !wasSeen(fnv(s.icao24, s.callsign, 'A')) &&
            markSeen(fnv(s.icao24, s.callsign, 'D')))
            count(s, false, hour);
    }
}

void DailyStats::count(const StateVector &s, bool arrival, int hour)
{
    if (arrival) { ++_arrivals;   if (hour >= 0) ++_arrHour[hour]; }
    else         { ++_departures; if (hour >= 0) ++_depHour[hour]; }
    _dirty = true;

    const String airline = airlineOf(s.callsign);
    if (airline.length() && (_airlines.count(airline) || _airlines.size() < kMaxAirlines))
        ++_airlines[airline];
    String type = s.aircraft_type;
    type.trim();
    if (type.length() && (_types.count(type) || _types.size() < kMaxTypes))
    {
        TypeCount &t = _types[type];
        ++t.n;
        t.seq = ++_seq;
    }
}

String DailyStats::busiestAirline(int &count) const
{
    String best;
    count = 0;
    for (const auto &kv : _airlines)
        if (kv.second > count) { best = kv.first; count = kv.second; }
    return best;
}

String DailyStats::rarestType() const
{
    String best;
    int fewest = 1 << 30, latest = -1;
    for (const auto &kv : _types)
        if (kv.second.n < fewest || (kv.second.n == fewest && kv.second.seq > latest))
        {
            best = kv.first; fewest = kv.second.n; latest = kv.second.seq;
        }
    return best;
}

// ── Persistence ─────────────────────────────────────────────────────────────

void DailyStats::serialise(std::vector<uint8_t> &v)
{
    v.clear();
    v.reserve(sizeof(_seen) + 600);
    put(v, kMagic);
    put(v, (int16_t)_year); put(v, (int16_t)_day);
    put(v, _arrivals); put(v, _departures); put(v, _goArounds);
    for (uint16_t n : _arrHour) put(v, n);
    for (uint16_t n : _depHour) put(v, n);
    put(v, _seenCount); put(v, _seq);
    v.insert(v.end(), (const uint8_t *)_seen, (const uint8_t *)_seen + sizeof(_seen));
    put(v, (uint8_t)_airlines.size());
    for (const auto &kv : _airlines)
    {
        char code[4] = {};
        strlcpy(code, kv.first.c_str(), sizeof(code));
        v.insert(v.end(), code, code + 4);
        put(v, kv.second);
    }
    put(v, (uint8_t)_types.size());
    for (const auto &kv : _types)
    {
        char t[6] = {};
        strlcpy(t, kv.first.c_str(), sizeof(t));
        v.insert(v.end(), t, t + 6);
        put(v, kv.second.n); put(v, kv.second.seq);
    }
    _dirty = false;
}

bool DailyStats::writeFile(const std::vector<uint8_t> &data)
{
    File f = LittleFS.open("/stats.tmp", "w");
    if (!f) return false;
    const bool ok = f.write(data.data(), data.size()) == data.size();
    f.close();
    if (!ok) { LittleFS.remove("/stats.tmp"); return false; }
    LittleFS.remove(kPath);
    return LittleFS.rename("/stats.tmp", kPath);
}

void DailyStats::load()
{
    File f = LittleFS.open(kPath, "r");
    if (!f) return;
    uint32_t magic = 0;
    int16_t year = 0, day = 0;
    bool ok = get(f, magic) && magic == kMagic && get(f, year) && get(f, day) &&
              get(f, _arrivals) && get(f, _departures) && get(f, _goArounds);
    for (uint16_t &n : _arrHour) ok = ok && get(f, n);
    for (uint16_t &n : _depHour) ok = ok && get(f, n);
    ok = ok && get(f, _seenCount) && get(f, _seq) &&
         f.read((uint8_t *)_seen, sizeof(_seen)) == sizeof(_seen);
    uint8_t n = 0;
    ok = ok && get(f, n);
    for (int i = 0; ok && i < n; ++i)
    {
        char code[5] = {};
        uint16_t c = 0;
        ok = f.read((uint8_t *)code, 4) == 4 && get(f, c);
        if (ok) _airlines[String(code)] = c;
    }
    ok = ok && get(f, n);
    for (int i = 0; ok && i < n; ++i)
    {
        char t[7] = {};
        TypeCount tc{};
        ok = f.read((uint8_t *)t, 6) == 6 && get(f, tc.n) && get(f, tc.seq);
        if (ok) _types[String(t)] = tc;
    }
    f.close();
    if (!ok)
    {
        // Damaged or from an older version: start clean.
        reset();
        Log.println("Stats: saved file unreadable, starting fresh");
        return;
    }
    _year = year;
    _day  = day;   // rollDay() resets it if this is not today
    Log.printf("Stats: restored %u arrivals, %u departures\n", _arrivals, _departures);
}
