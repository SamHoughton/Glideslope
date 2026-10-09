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
    // Yesterday only if the counts are from the day before (not a board
    // switched off for a week).
    const bool consecutive = _day >= 0 && ((lt.tm_year == _year && lt.tm_yday == _day + 1) ||
                                           (lt.tm_year == _year + 1 && lt.tm_yday == 0 && _day >= 364));
    _hasYesterday = consecutive;
    for (int h = 0; h < 24; ++h) _yHour[h] = consecutive ? _arrHour[h] + _depHour[h] : 0;
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
    _nAirlines = _nTypes = 0;
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
    if (airline.length()) addAirline(airline.c_str(), 1);
    String type = s.aircraft_type;
    type.trim();
    if (type.length())
        if (TypeCount *t = typeSlot(type.c_str(), true)) { ++t->n; t->seq = ++_seq; }
}

void DailyStats::addAirline(const char *code, uint16_t n)
{
    for (uint8_t i = 0; i < _nAirlines; ++i)
        if (strncmp(_airlines[i].code, code, sizeof(_airlines[i].code)) == 0) { _airlines[i].n += n; return; }
    if (_nAirlines >= kMaxAirlines) return;
    strlcpy(_airlines[_nAirlines].code, code, sizeof(_airlines[0].code));
    _airlines[_nAirlines++].n = n;
}

DailyStats::TypeCount *DailyStats::typeSlot(const char *t, bool create)
{
    for (uint8_t i = 0; i < _nTypes; ++i)
        if (strncmp(_types[i].t, t, sizeof(_types[i].t)) == 0) return &_types[i];
    if (!create || _nTypes >= kMaxTypes) return nullptr;
    TypeCount &slot = _types[_nTypes++];
    slot = TypeCount{};
    strlcpy(slot.t, t, sizeof(slot.t));
    return &slot;
}

String DailyStats::busiestAirline(int &count) const
{
    String best;
    count = 0;
    for (uint8_t i = 0; i < _nAirlines; ++i)
        if (_airlines[i].n > count) { best = _airlines[i].code; count = _airlines[i].n; }
    return best;
}

String DailyStats::rarestType() const
{
    String best;
    int fewest = 1 << 30, latest = -1;
    for (uint8_t i = 0; i < _nTypes; ++i)
    {
        const TypeCount &tc = _types[i];
        if (tc.n < fewest || (tc.n == fewest && tc.seq > latest))
        {
            best = String(tc.t).substring(0, sizeof(tc.t)); fewest = tc.n; latest = tc.seq;
        }
    }
    return best;
}

// ── Persistence ─────────────────────────────────────────────────────────────

int DailyStats::yesterdayUpTo(int hour, int minute) const
{
    if (!_hasYesterday) return 0;
    float n = 0;
    for (int h = 0; h < hour && h < 24; ++h) n += _yHour[h];
    if (hour >= 0 && hour < 24) n += _yHour[hour] * minute / 60.0f;
    return (int)lroundf(n);
}

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
    put(v, _nAirlines);
    for (uint8_t i = 0; i < _nAirlines; ++i)
    {
        v.insert(v.end(), _airlines[i].code, _airlines[i].code + 4);
        put(v, _airlines[i].n);
    }
    put(v, _nTypes);
    for (uint8_t i = 0; i < _nTypes; ++i)
    {
        v.insert(v.end(), _types[i].t, _types[i].t + 6);
        put(v, _types[i].n); put(v, _types[i].seq);
    }
    // Yesterday (added in 4.0: older files simply end before it).
    put(v, (uint8_t)(_hasYesterday ? 1 : 0));
    for (uint16_t n : _yHour) put(v, n);
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
        if (ok) addAirline(code, c);
    }
    ok = ok && get(f, n);
    for (int i = 0; ok && i < n; ++i)
    {
        char t[7] = {};
        uint16_t tn = 0, tseq = 0;
        ok = f.read((uint8_t *)t, 6) == 6 && get(f, tn) && get(f, tseq);
        if (ok)
            if (TypeCount *slot = typeSlot(t, true)) { slot->n = tn; slot->seq = tseq; }
    }
    if (ok)
    {
        uint8_t y = 0;
        bool yok = get(f, y);
        uint16_t hours[24];
        for (uint16_t &h : hours) yok = yok && get(f, h);
        if (yok && y)
        {
            _hasYesterday = true;
            memcpy(_yHour, hours, sizeof(_yHour));
        }
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
