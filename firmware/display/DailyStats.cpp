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
    constexpr uint32_t    kExtraMagic = 0x37535347;   // "GSS7": the 7.1 additions follow

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
    _lastArr = 0; _gapSum = 0; _gapN = 0; _tightSec = 0; _tightMin = 0; _tightId[0] = '\0';
    _nRecent = 0; _nRwy = 0;
    _nSpots = 0; _spotHead = 0; _spotsToday = 0;
}

// ── Runway rhythm ───────────────────────────────────────────────────────────

void DailyStats::noteArrival(const StateVector &s, const char *runway)
{
    const time_t now = time(nullptr);
    if (runway && runway[0])
    {
        int i = 0;
        while (i < _nRwy && strncmp(_rwy[i].name, runway, 3) != 0) ++i;
        if (i == _nRwy && _nRwy < 4) { strlcpy(_rwy[i].name, runway, sizeof(_rwy[i].name)); _rwy[i].n = 0; ++_nRwy; }
        if (i < _nRwy) ++_rwy[i].n;
    }
    if (now < 1600000000) return;
    if (_lastArr)
    {
        const uint32_t gap = (uint32_t)now - _lastArr;
        // Under a minute is two aircraft bunching as they join final (they are
        // timed there, not at the threshold), not a real gap on one runway;
        // over 15 minutes is a lull.
        if (gap >= 60 && gap <= 900)
        {
            _gapSum += gap; ++_gapN;
            memmove(_recent + 1, _recent, sizeof(_recent) - sizeof(_recent[0]));
            _recent[0] = (uint16_t)gap;
            if (_nRecent < 8) ++_nRecent;
            if (!_tightSec || gap < _tightSec)
            {
                _tightSec = (uint16_t)gap;
                struct tm lt;
                localtime_r(&now, &lt);
                _tightMin = (uint16_t)(lt.tm_hour * 60 + lt.tm_min);
                String id = s.callsign;
                id.trim();
                strlcpy(_tightId, id.c_str(), sizeof(_tightId));
            }
        }
        else if (gap > 900)
            _nRecent = 0;   // after a lull the recent rhythm starts again
    }
    _lastArr = (uint32_t)now;
}

int DailyStats::recentGapSec() const
{
    if (_nRecent < 3) return 0;
    // Recent only: nothing if the last arrival was a while ago.
    const time_t now = time(nullptr);
    if (now > 1600000000 && _lastArr && (uint32_t)now - _lastArr > 900) return 0;
    uint32_t sum = 0;
    for (int i = 0; i < _nRecent; ++i) sum += _recent[i];
    return (int)(sum / _nRecent);
}

int DailyStats::runways(RunwayCount *out, int cap) const
{
    int n = 0;
    for (int i = 0; i < _nRwy && i < 4; ++i)
    {
        int j = n < cap ? n : cap - 1;
        if (n == cap && _rwy[i].n <= out[cap - 1].n) continue;
        while (j > 0 && out[j - 1].n < _rwy[i].n) { out[j] = out[j - 1]; --j; }
        out[j] = _rwy[i];
        if (n < cap) ++n;
    }
    return n;
}

int DailyStats::busiestHour(int &movements) const
{
    int best = -1;
    movements = 0;
    for (int h = 0; h < 24; ++h)
        if (_arrHour[h] + _depHour[h] > movements) { movements = _arrHour[h] + _depHour[h]; best = h; }
    return best;
}

// ── Spotter log ─────────────────────────────────────────────────────────────

void DailyStats::noteSpot(SpotKind kind, const char *ident, const char *what)
{
    rollDay();
    if (!ident || !ident[0]) return;
    if (kind != GoAround && kind != Emergency)
        for (int i = 0; i < _nSpots; ++i)
        {
            const Spot &o = spot(i);
            if (o.kind == kind && strncmp(o.ident, ident, sizeof(o.ident) - 1) == 0) return;
        }
    struct tm lt;
    const time_t now = time(nullptr);
    uint16_t minute = 0;
    if (now > 1600000000) { localtime_r(&now, &lt); minute = (uint16_t)(lt.tm_hour * 60 + lt.tm_min); }
    ++_spotsToday;
    _dirty = true;
    if (_nSpots >= kMaxSpots)
    {
        // Full: make room by dropping the oldest overhead pass; if there is
        // none, the oldest entry, unless the new one is only an overhead
        // pass, which then isn't kept (it is still counted).
        int drop = -1;
        for (int i = _nSpots - 1; i >= 0 && drop < 0; --i)
            if (_spots[i].kind == Overhead) drop = i;
        if (drop < 0)
        {
            if (kind == Overhead) return;
            drop = _nSpots - 1;
        }
        memmove(&_spots[drop], &_spots[drop + 1], (size_t)(_nSpots - 1 - drop) * sizeof(Spot));
        --_nSpots;
    }
    memmove(&_spots[1], &_spots[0], (size_t)_nSpots * sizeof(Spot));
    Spot &s = _spots[0];
    s.minute = minute;
    s.kind = kind;
    strlcpy(s.ident, ident, sizeof(s.ident));
    strlcpy(s.what, what ? what : "", sizeof(s.what));
    ++_nSpots;
    _spotHead = 0;
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
        watchGoArounds(s, st, onFinal, (uint32_t)millis());
        if (onFinal)
        {
            if (markSeen(fnv(s.icao24, s.callsign, 'A')))
            {
                count(s, true, hour);
                noteArrival(s, st.runway);
            }
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
    {
        const time_t now = time(nullptr);
        if (now > 1600000000) _lastMove = now;
    }
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

namespace
{
    // Insert (code, n) into out[0..len) kept in descending order of n.
    void insertTop(DailyStats::Top *out, int &len, int cap, const char *code, size_t codeLen, int n)
    {
        int i = len < cap ? len : cap - 1;
        if (len == cap && n <= out[cap - 1].n) return;
        while (i > 0 && out[i - 1].n < n) { out[i] = out[i - 1]; --i; }
        memset(out[i].code, 0, sizeof(out[i].code));
        memcpy(out[i].code, code, min(codeLen, sizeof(out[i].code) - 1));
        out[i].n = n;
        if (len < cap) ++len;
    }
}

int DailyStats::topAirlines(Top *out, int cap) const
{
    int len = 0;
    for (uint8_t i = 0; i < _nAirlines; ++i)
        insertTop(out, len, cap, _airlines[i].code, strnlen(_airlines[i].code, sizeof(_airlines[i].code)), _airlines[i].n);
    return len;
}

int DailyStats::topTypes(Top *out, int cap) const
{
    int len = 0;
    for (uint8_t i = 0; i < _nTypes; ++i)
        insertTop(out, len, cap, _types[i].t, strnlen(_types[i].t, sizeof(_types[i].t)), _types[i].n);
    return len;
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

size_t DailyStats::serialisedSize() const
{
    return sizeof(kMagic) + 2 * sizeof(int16_t) + 3 * sizeof(uint16_t) + sizeof(_arrHour) + sizeof(_depHour) +
           2 * sizeof(uint16_t) + sizeof(_seen) + 1 + _nAirlines * (4 + sizeof(uint16_t)) +
           1 + _nTypes * (6 + 2 * sizeof(uint16_t)) + 1 + sizeof(_yHour) +
           kExtraSize;
}

void DailyStats::serialise(std::vector<uint8_t> &v)
{
    // Reserved exactly: growing it needed a second, larger block while the
    // first was held (4.0.2 candidate crashed there with a TLS session open).
    v.clear();
    v.reserve(serialisedSize());
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
    // The rhythm and the spotter log (added in 7.1).
    put(v, kExtraMagic);
    put(v, _lastArr); put(v, _gapSum); put(v, _gapN); put(v, _tightSec); put(v, _tightMin);
    v.insert(v.end(), _tightId, _tightId + sizeof(_tightId));
    for (uint16_t g : _recent) put(v, g);
    put(v, _nRecent); put(v, _nRwy);
    v.insert(v.end(), (const uint8_t *)_rwy, (const uint8_t *)_rwy + sizeof(_rwy));
    v.insert(v.end(), (const uint8_t *)_spots, (const uint8_t *)_spots + sizeof(_spots));
    put(v, _nSpots); put(v, _spotHead); put(v, _spotsToday);
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
        uint32_t m2 = 0;
        if (yok && get(f, m2) && m2 == kExtraMagic)
        {
            bool e = get(f, _lastArr) && get(f, _gapSum) && get(f, _gapN) && get(f, _tightSec) && get(f, _tightMin) &&
                     f.read((uint8_t *)_tightId, sizeof(_tightId)) == sizeof(_tightId);
            for (uint16_t &g : _recent) e = e && get(f, g);
            e = e && get(f, _nRecent) && get(f, _nRwy) &&
                f.read((uint8_t *)_rwy, sizeof(_rwy)) == sizeof(_rwy) &&
                f.read((uint8_t *)_spots, sizeof(_spots)) == sizeof(_spots) &&
                get(f, _nSpots) && get(f, _spotHead) && get(f, _spotsToday);
            _tightId[sizeof(_tightId) - 1] = '\0';
            if (!e || _nRwy > 4 || _nSpots > kMaxSpots || _spotHead >= kMaxSpots || _nRecent > 8)
            {
                _lastArr = 0; _gapSum = 0; _gapN = 0; _tightSec = 0; _nRecent = 0; _nRwy = 0;
                _nSpots = 0; _spotHead = 0; _spotsToday = 0; _tightId[0] = '\0';
            }
            if (_tightSec && _tightSec < 60) { _tightSec = 0; _tightId[0] = 0; }   // saved before 7.1.0's one-minute floor
            for (Spot &sp : _spots) { sp.ident[sizeof(sp.ident) - 1] = '\0'; sp.what[sizeof(sp.what) - 1] = '\0'; }
            if (_spotHead && _nSpots)
            {
                // Saved by 7.1-8.0 as a ring (newest at head - 1): newest first now.
                Spot ordered[kMaxSpots];
                for (int i = 0; i < _nSpots; ++i) ordered[i] = _spots[(_spotHead + kMaxSpots - 1 - i) % kMaxSpots];
                memcpy(_spots, ordered, sizeof(Spot) * _nSpots);
                _spotHead = 0;
            }
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

// ── Go-arounds, from every aircraft in range ───────────────────────────────

void DailyStats::watchGoArounds(const StateVector &s, const ApproachStatus &st, bool onFinal, uint32_t now)
{
    const uint32_t id = (uint32_t)strtoul(s.icao24.c_str(), nullptr, 16) | 1;
    const float alt = isnan(s.baro_altitude) ? NAN : (float)(s.baro_altitude * 3.28084);
    FinalWatch *slot = nullptr;
    for (FinalWatch &w : _watch) if (w.id == id) { slot = &w; break; }
    if (onFinal)
    {
        // On final: remember it (the slot it had, else the stalest).
        if (!slot)
        {
            slot = &_watch[0];
            for (FinalWatch &w : _watch) if (now - w.finalMs > now - slot->finalMs) slot = &w;
            slot->id = id;
        }
        slot->finalMs = now;
        slot->altFt = isnan(alt) ? 0 : (int16_t)alt;
        strlcpy(slot->rwy, st.runway, sizeof(slot->rwy));
        return;
    }
    if (!slot || now - slot->finalMs > 180000) return;
    // Off final within three minutes, below 5,000 ft, climbing hard and
    // higher than when it was last on final: it has gone around.
    const float vr = isnan(s.vertical_rate) ? NAN : (float)(s.vertical_rate * 196.85);
    if (!isnan(vr) && vr > 500 && !isnan(alt) && alt < 5000 && alt > slot->altFt + 100)
    {
        String cs = s.callsign;
        cs.trim();
        strlcpy(_gaIdent, cs.length() ? cs.c_str() : s.icao24.c_str(), sizeof(_gaIdent));
        strlcpy(_gaRwy, slot->rwy, sizeof(_gaRwy));
        _gaAlt = (int16_t)alt;
        _gaPending = true;
        ++_goArounds;
        noteSpot(GoAround, _gaIdent, _gaRwy);   // marks the stats dirty too
        slot->id = 0;                           // once
    }
}

bool DailyStats::takeGoAround(char *ident, size_t identLen, char *runway, size_t rwyLen, int &altFt)
{
    if (!_gaPending) return false;
    _gaPending = false;
    strlcpy(ident, _gaIdent, identLen);
    strlcpy(runway, _gaRwy, rwyLen);
    altFt = _gaAlt;
    return true;
}
