#include "display/DailyStats.h"
#include <time.h>

void DailyStats::rollDay()
{
    const time_t now = time(nullptr);
    if (now < 1600000000) return;   // clock not synced yet: keep counting
    struct tm lt;
    localtime_r(&now, &lt);
    if (lt.tm_yday != _day)
    {
        _day = lt.tm_yday;
        _arrivals = 0;
        _departures = 0;
        _goArounds = 0;
        memset(_seen, 0, sizeof(_seen));
        _seenCount = 0;
        _airlines.clear();
        _types.clear();
        _typeLastMs.clear();
    }
}

void DailyStats::note(const FlightInfo &f)
{
    rollDay();
    const bool toLhr   = f.destination.code_icao == "EGLL" || f.destination.code_iata == "LHR";
    const bool fromLhr = f.origin.code_icao == "EGLL" || f.origin.code_iata == "LHR";
    if ((!toLhr && !fromLhr) || f.ident.length() == 0 || !markSeen(f.ident)) return;
    if (toLhr) ++_arrivals; else ++_departures;

    String airline = f.ident_iata.length() >= 2 ? f.ident_iata.substring(0, 2) : f.operator_icao;
    airline.trim();
    if (airline.length()) ++_airlines[airline];
    String type = f.aircraft_code;
    type.trim();
    if (type.length())
    {
        ++_types[type];
        _typeLastMs[type] = millis();
    }
}

bool DailyStats::markSeen(const String &ident)
{
    // FNV-1a; 0 is reserved for empty slots.
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < ident.length(); ++i) { h ^= (uint8_t)ident[i]; h *= 16777619u; }
    if (h == 0) h = 1;
    if (_seenCount >= kSeenSlots * 3 / 4) return false;   // full for today: stop counting
    for (size_t i = h % kSeenSlots;; i = (i + 1) % kSeenSlots)
    {
        if (_seen[i] == h) return false;
        if (_seen[i] == 0) { _seen[i] = h; ++_seenCount; return true; }
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
    int fewest = 1 << 30;
    unsigned long latest = 0;
    for (const auto &kv : _types)
    {
        const unsigned long seen = _typeLastMs.count(kv.first) ? _typeLastMs.at(kv.first) : 0;
        if (kv.second < fewest || (kv.second == fewest && seen > latest))
        {
            best = kv.first; fewest = kv.second; latest = seen;
        }
    }
    return best;
}
