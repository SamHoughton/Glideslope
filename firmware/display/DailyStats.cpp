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
        _goArounds = 0;
        _counted.clear();
        _airlines.clear();
        _types.clear();
        _typeLastMs.clear();
    }
}

void DailyStats::note(const FlightInfo &f)
{
    rollDay();
    const bool toLhr = f.destination.code_icao == "EGLL" || f.destination.code_iata == "LHR";
    if (!toLhr || f.ident.length() == 0 || _counted.count(f.ident)) return;
    if (_counted.size() > 1500) return;   // bounded; far above a day's arrivals
    _counted[f.ident] = true;
    ++_arrivals;

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
