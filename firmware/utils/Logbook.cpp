#include "utils/Logbook.h"
#include "utils/Airframes.h"
#include "utils/SeenLog.h"
#include "config/Countries.h"
#include "utils/TelnetLogger.h"
#include <LittleFS.h>
#include <WiFiClient.h>
#include <time.h>

namespace
{
    constexpr const char *kDir = "/log", *kDays = "/log/days.bin", *kEvents = "/log/events.bin",
                         *kCollection = "/log/collection.bin", *kRecords = "/log/records.bin";

    // One airline or type in the collection. 20 bytes.
    struct Item
    {
        char     code[6];
        uint8_t  kind;          // 0 airline, 1 type
        uint8_t  spare;
        uint16_t firstDay, lastDay, days, spare2;   // days since 2020-01-01
        uint32_t count;
    };
    static_assert(sizeof(Item) == 20, "collection items are 20 bytes on disk");

    Logbook::Records s_rec;

    long daysFromCivil(int y, int m, int d)
    {
        y -= m <= 2;
        const long era = (y >= 0 ? y : y - 399) / 400;
        const unsigned yoe = (unsigned)(y - era * 400);
        const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        return era * 146097 + (long)(yoe * 365 + yoe / 4 - yoe / 100 + doy) - 719468;
    }
    uint16_t dayNumber(uint32_t ymd)   // days since 2020-01-01
    {
        return (uint16_t)(daysFromCivil(ymd / 10000, ymd / 100 % 100, ymd % 100) - daysFromCivil(2020, 1, 1));
    }
    void ymdText(uint32_t ymd, char *out, size_t len)
    {
        snprintf(out, len, "%04u-%02u-%02u", (unsigned)(ymd / 10000), (unsigned)(ymd / 100 % 100), (unsigned)(ymd % 100));
    }
    void dayText(uint16_t n, char *out, size_t len)
    {
        const time_t t = (time_t)(daysFromCivil(2020, 1, 1) + n) * 86400L + 43200;
        struct tm g;
        gmtime_r(&t, &g);
        snprintf(out, len, "%04d-%02d-%02d", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday);
    }

    void saveRecords()
    {
        File f = LittleFS.open(kRecords, "w");
        if (!f) return;
        f.write((const uint8_t *)&s_rec, sizeof(s_rec));
        f.close();
    }

    // Writes to a client for at most 8 s (a phone that stops reading).
    struct Writer
    {
        WiFiClient &c;
        uint32_t start = millis();
        bool ok = true;
        explicit Writer(WiFiClient &cl) : c(cl) {}
        void line(const char *s)
        {
            if (!ok) return;
            if (!c.connected() || millis() - start > 8000) { ok = false; return; }
            c.print(s);
        }
    };

    void csvHeader(WiFiClient &c, const char *name)
    {
        c.printf("HTTP/1.1 200 OK\r\nContent-Type: text/csv; charset=utf-8\r\n"
                 "Content-Disposition: inline; filename=\"glideslope-%s.csv\"\r\n"
                 "Cache-Control: no-store\r\nConnection: close\r\n\r\n", name);
    }

    void clean(const char *s, char *out, size_t len)   // letters, digits, dashes, spaces
    {
        size_t n = 0;
        for (; *s && n + 1 < len; ++s)
            if (isalnum((unsigned char)*s) || *s == '-' || *s == ' ') out[n++] = *s;
        out[n] = 0;
    }

    const char *const kKinds[] = {"rare", "first visit", "watchlist", "overhead", "go-around", "emergency", "crosswind"};
}

namespace
{
    // Before 9.1.1 an aircraft climbing out after a go-around could be
    // counted again and again (BA876 seventeen times). Each start, drop a
    // go-around by the same flight within ten minutes of one kept; once
    // there are none, the file is only read.
    void dropRepeatGoArounds()
    {
        constexpr int kKind = 4;            // DailyStats::GoAround
        constexpr int kRecent = 8;
        File in = LittleFS.open(kEvents, "r");
        if (!in) return;
        Logbook::Event e, kept[kRecent] = {};
        int nKept = 0, dropped = 0;
        File out;
        size_t at = 0;
        while (in.read((uint8_t *)&e, sizeof(e)) == sizeof(e))
        {
            bool repeat = false;
            if (e.kind == kKind)
            {
                for (int i = 0; i < nKept && !repeat; ++i)
                    repeat = strncmp(kept[i].ident, e.ident, sizeof(e.ident)) == 0 && e.at - kept[i].at < 600;
                if (!repeat) { memmove(&kept[1], &kept[0], sizeof(kept[0]) * (kRecent - 1)); kept[0] = e; nKept = min(nKept + 1, kRecent); }
            }
            if (repeat && !out)
            {
                // The first repeat: copy what came before it, then go on.
                out = LittleFS.open("/log/events.tmp", "w");
                if (!out) { in.close(); return; }
                File again = LittleFS.open(kEvents, "r");
                Logbook::Event x;
                for (size_t i = 0; i < at && again.read((uint8_t *)&x, sizeof(x)) == sizeof(x); ++i)
                    out.write((const uint8_t *)&x, sizeof(x));
                again.close();
            }
            ++at;
            if (repeat) ++dropped;
            else if (out) out.write((const uint8_t *)&e, sizeof(e));
        }
        in.close();
        if (!out) return;
        out.close();
        LittleFS.remove(kEvents);
        LittleFS.rename("/log/events.tmp", kEvents);
        Log.printf("Logbook: removed %d repeated go-arounds\n", dropped);
    }
}

int Logbook::goAroundsSince(uint32_t t)
{
    File f = LittleFS.open(kEvents, "r");
    if (!f) return 0;
    Event e;
    int n = 0;
    while (f.read((uint8_t *)&e, sizeof(e)) == sizeof(e)) n += e.kind == 4 && e.at >= t;
    f.close();
    return n;
}

void Logbook::begin()
{
    if (!LittleFS.exists(kDir)) LittleFS.mkdir(kDir);
    dropRepeatGoArounds();
    File f = LittleFS.open(kRecords, "r");
    if (f && f.read((uint8_t *)&s_rec, sizeof(s_rec)) != sizeof(s_rec)) s_rec = Records();
    if (f) f.close();
    Log.printf("Logbook: %u days, %u events, %u airlines and %u types; busiest day %u\n",
               (unsigned)(LittleFS.exists(kDays) ? LittleFS.open(kDays, "r").size() / sizeof(Day) : 0),
               (unsigned)(LittleFS.exists(kEvents) ? LittleFS.open(kEvents, "r").size() / sizeof(Event) : 0),
               s_rec.airlines, s_rec.types, s_rec.bestDay);
}

const Logbook::Records &Logbook::records() { return s_rec; }

void Logbook::addEvent(const Event &e)
{
    File f = LittleFS.open(kEvents, "a");
    if (!f) return;
    f.write((const uint8_t *)&e, sizeof(e));
    f.close();
}

void Logbook::addDay(const Day &d, const std::vector<Tally> &tallies, char *line1, size_t len1, char *line2, size_t len2)
{
    line1[0] = line2[0] = 0;
    if (!d.date) return;
    // The day: appended, or replacing the last if it is the same day again
    // (a restart just after midnight); then the collection and records were
    // already counted, so they are left alone.
    bool again = false;
    {
        File f = LittleFS.open(kDays, LittleFS.exists(kDays) ? "r+" : "w");
        if (!f) return;
        size_t at = f.size() / sizeof(Day) * sizeof(Day);
        if (at >= sizeof(Day))
        {
            Day last;
            f.seek(at - sizeof(Day));
            if (f.read((uint8_t *)&last, sizeof(last)) == sizeof(last) && last.date == d.date) { at -= sizeof(Day); again = true; }
        }
        f.seek(at);
        f.write((const uint8_t *)&d, sizeof(d));
        f.close();
    }
    if (!s_rec.since) s_rec.since = d.date;
    if (again) return;

    // The records.
    const uint16_t moves = d.arrivals + d.departures;
    char when[12];
    ymdText(d.date, when, sizeof(when));
    if (moves > s_rec.bestDay)
    {
        if (s_rec.bestDay)
        {
            snprintf(line1, len1, "New record");
            snprintf(line2, len2, "%u movements", (unsigned)moves);
        }
        s_rec.bestDay = moves;
        s_rec.bestDayDate = d.date;
    }
    if (d.windiest > s_rec.windiest) s_rec.windiest = d.windiest;   // (9.1: announced live, not at midnight)
    if (d.goArounds > s_rec.bestGa)
    {
        if (!line1[0] && s_rec.bestGa) { snprintf(line1, len1, "Most go-arounds"); snprintf(line2, len2, "%u in a day", d.goArounds); }
        s_rec.bestGa = d.goArounds;
        s_rec.bestGaDate = d.date;
    }
    if (d.tightestSec && (!s_rec.tightest || d.tightestSec < s_rec.tightest))
    {
        s_rec.tightest = d.tightestSec;
        s_rec.tightDate = d.date;
    }

    // The collection: one pass over the file, the day's tallies matched in
    // memory (a day has at most ~130), then the new ones appended.
    const uint16_t today = dayNumber(d.date);
    std::vector<bool> found(tallies.size(), false);
    {
        File f = LittleFS.open(kCollection, LittleFS.exists(kCollection) ? "r+" : "w");
        if (f)
        {
            Item chunk[16];
            size_t pos = 0;
            for (;;)
            {
                f.seek(pos);
                const int got = (int)(f.read((uint8_t *)chunk, sizeof(chunk)) / sizeof(Item));
                if (got <= 0) break;
                bool changed = false;
                for (int i = 0; i < got; ++i)
                    for (size_t t = 0; t < tallies.size(); ++t)
                        if (!found[t] && chunk[i].kind == tallies[t].kind && strncmp(chunk[i].code, tallies[t].code, 6) == 0)
                        {
                            found[t] = true;
                            if (chunk[i].lastDay != today) { ++chunk[i].days; chunk[i].lastDay = today; }
                            chunk[i].count += tallies[t].n;
                            changed = true;
                            break;
                        }
                if (changed) { f.seek(pos); f.write((const uint8_t *)chunk, got * sizeof(Item)); }
                pos += got * sizeof(Item);
                if (got < 16) break;
            }
            const uint16_t airlinesBefore = s_rec.airlines, typesBefore = s_rec.types;
            f.seek(pos);
            for (size_t t = 0; t < tallies.size(); ++t)
            {
                if (found[t]) continue;
                Item it{};
                strlcpy(it.code, tallies[t].code, sizeof(it.code));
                it.kind = tallies[t].kind;
                it.firstDay = it.lastDay = today;
                it.days = 1;
                it.count = tallies[t].n;
                f.write((const uint8_t *)&it, sizeof(it));
                if (it.kind == 0) ++s_rec.airlines; else ++s_rec.types;
            }
            f.close();
            // Milestones: every 25th airline or type.
            if (!line1[0] && s_rec.airlines / 25 > airlinesBefore / 25 && airlinesBefore)
            {
                snprintf(line1, len1, "Milestone");
                snprintf(line2, len2, "%u airlines seen", (unsigned)(s_rec.airlines / 25 * 25));
            }
            else if (!line1[0] && s_rec.types / 25 > typesBefore / 25 && typesBefore)
            {
                snprintf(line1, len1, "Milestone");
                snprintf(line2, len2, "%u aircraft types", (unsigned)(s_rec.types / 25 * 25));
            }
        }
    }
    saveRecords();
    Log.printf("Logbook: %s, %u movements; %u airlines and %u types in all%s%s\n", when, (unsigned)moves,
               s_rec.airlines, s_rec.types, line1[0] ? "; " : "", line1);
}

void Logbook::writeDaysCsv(WiFiClient &c)
{
    csvHeader(c, "days");
    Writer w(c);
    w.line("date,arrivals,departures,go_arounds,busiest_airline,busiest_n,tightest_s,average_gap_s,runway,runway_n,airlines,types");
    for (int h = 0; h < 24; ++h) { char b[8]; snprintf(b, sizeof(b), ",h%02d", h); w.line(b); }
    w.line("\n");
    File f = LittleFS.open(kDays, "r");
    if (!f) return;
    Day d;
    char line[200], ymd[12], a[8], r[8];
    while (w.ok && f.read((uint8_t *)&d, sizeof(d)) == sizeof(d))
    {
        ymdText(d.date, ymd, sizeof(ymd));
        clean(d.busiest, a, sizeof(a));
        clean(d.runway, r, sizeof(r));
        int n = snprintf(line, sizeof(line), "%s,%u,%u,%u,%s,%u,%u,%u,%s,%u,%u,%u", ymd, d.arrivals, d.departures,
                         d.goArounds, a, d.busiestN, d.tightestSec, d.avgGapSec, r, d.runwayN, d.airlines, d.types);
        for (int h = 0; h < 24 && n < (int)sizeof(line) - 6; ++h) n += snprintf(line + n, sizeof(line) - n, ",%u", d.hours[h]);
        snprintf(line + n, sizeof(line) - n, "\n");
        w.line(line);
    }
    f.close();
}

void Logbook::writeEventsCsv(WiFiClient &c)
{
    csvHeader(c, "events");
    Writer w(c);
    w.line("time,kind,flight,detail\n");
    File f = LittleFS.open(kEvents, "r");
    if (!f) return;
    Event e;
    char line[96], id[10], what[12], when[24];
    while (w.ok && f.read((uint8_t *)&e, sizeof(e)) == sizeof(e))
    {
        const time_t t = e.at;
        struct tm lt;
        localtime_r(&t, &lt);
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &lt);
        clean(e.ident, id, sizeof(id));
        clean(e.what, what, sizeof(what));
        snprintf(line, sizeof(line), "%s,%s,%s,%s\n", e.at ? when : "", e.kind < 6 ? kKinds[e.kind < 7 ? e.kind : 0] : "other", id, what);
        w.line(line);
    }
    f.close();
}

void Logbook::writeCollectionCsv(WiFiClient &c)
{
    csvHeader(c, "collection");
    Writer w(c);
    w.line("kind,code,first_seen,last_seen,days,count\n");
    File f = LittleFS.open(kCollection, "r");
    if (!f) return;
    Item it;
    char line[80], code[8], first[12], last[12];
    while (w.ok && f.read((uint8_t *)&it, sizeof(it)) == sizeof(it))
    {
        clean(it.code, code, sizeof(code));
        dayText(it.firstDay, first, sizeof(first));
        dayText(it.lastDay, last, sizeof(last));
        static const char *const kItemKinds[] = {"airline", "type", "airport"};
        snprintf(line, sizeof(line), "%s,%s,%s,%s,%u,%lu\n", kItemKinds[it.kind < 3 ? it.kind : 1], code, first, last,
                 it.days, (unsigned long)it.count);
        w.line(line);
    }
    f.close();
}

size_t Logbook::bytesUsed()
{
    size_t n = 0;
    for (const char *p : {kDays, kEvents, kCollection, kRecords, Airframes::kPath})
        if (LittleFS.exists(p)) { File f = LittleFS.open(p, "r"); n += f.size(); f.close(); }
    return n;
}

bool Logbook::stickerIsNew(uint8_t kind, const char *code)
{
    if (!code || !code[0]) return false;
    const time_t now = time(nullptr);
    if (now < 1600000000) return false;
    const uint16_t today = SeenLog::dayNumber(now);
    File f = LittleFS.open(kCollection, "r");
    if (!f) return false;
    Item it;
    uint16_t first = today;
    bool found = false;
    while (!found && f.read((uint8_t *)&it, sizeof(it)) == sizeof(it))
    {
        if (it.kind != kind) continue;
        if (it.firstDay < first) first = it.firstDay;
        found = !strncmp(it.code, code, sizeof(it.code));
    }
    f.close();
    return !found && today - first >= 14;
}

int Logbook::noteOrigin(const char *icao, const char *ident)
{
    if (!icao || strlen(icao) != 4 || !ident || !ident[0]) return 0;
    const time_t now = time(nullptr);
    if (now < 1600000000) return 0;
    // Each flight once (the card's flight comes round every fetch).
    static char recent[12][9] = {};
    static uint8_t next = 0;
    for (const auto &r : recent)
        if (!strncmp(r, ident, 8)) return 0;
    strlcpy(recent[next], ident, sizeof(recent[next]));
    next = (next + 1) % 12;

    const uint16_t today = SeenLog::dayNumber(now);
    File f = LittleFS.open(kCollection, LittleFS.exists(kCollection) ? "r+" : "w");
    if (!f) return 0;
    Item it;
    size_t pos = 0;
    uint16_t firstAirportDay = today;   // when the board started collecting airports
    char iso[3], had[3];
    Countries::of(icao, iso, nullptr);
    bool countryKnown = !iso[0];        // unknown prefix: no country news
    while (f.read((uint8_t *)&it, sizeof(it)) == sizeof(it))
    {
        if (it.kind == 2 && it.firstDay < firstAirportDay) firstAirportDay = it.firstDay;
        if (it.kind == 2 && !countryKnown && Countries::of(it.code, had, nullptr) && !strcmp(had, iso)) countryKnown = true;
        if (it.kind == 2 && !strncmp(it.code, icao, 4))
        {
            if (it.lastDay != today) { ++it.days; it.lastDay = today; }
            ++it.count;
            f.seek(pos);
            f.write((const uint8_t *)&it, sizeof(it));
            f.close();
            return 0;
        }
        pos += sizeof(it);
    }
    it = Item{};
    strlcpy(it.code, icao, sizeof(it.code));
    it.kind = 2;
    it.firstDay = it.lastDay = today;
    it.days = 1;
    it.count = 1;
    f.seek(pos);
    f.write((const uint8_t *)&it, sizeof(it));
    f.close();
    ++s_rec.airports;
    saveRecords();
    Log.printf("Logbook: a first arrival from %s (%s)\n", icao, ident);
    // For the first fortnight nearly every airport is new: no news then.
    if (today - firstAirportDay < 14) return 0;
    return countryKnown ? 1 : 2;
}

void Logbook::writeAirframesCsv(WiFiClient &c)
{
    csvHeader(c, "airframes");
    Writer w(c);
    w.line("registration,type,airline,first_seen,last_seen,days\n");
    File f = LittleFS.open(Airframes::kPath, "r");
    if (!f) return;
    Airframes::Slot chunk[16];
    char line[80], reg[8], type[5], op[4], first[12], last[12];
    size_t got;
    while (w.ok && (got = f.read((uint8_t *)chunk, sizeof(chunk))) >= sizeof(Airframes::Slot))
        for (size_t i = 0; i < got / sizeof(Airframes::Slot) && w.ok; ++i)
        {
            const Airframes::Slot &s = chunk[i];
            if (!s.key) continue;
            memcpy(reg, s.reg, 7); reg[7] = 0;
            memcpy(type, s.type, 4); type[4] = 0;
            memcpy(op, s.op, 3); op[3] = 0;
            char r2[8], t2[5], o2[4];
            clean(reg, r2, sizeof(r2)); clean(type, t2, sizeof(t2)); clean(op, o2, sizeof(o2));
            dayText(s.firstDay, first, sizeof(first));
            dayText(s.lastDay, last, sizeof(last));
            snprintf(line, sizeof(line), "%s,%s,%s,%s,%s,%u\n", r2, t2, o2, first, last, s.days);
            w.line(line);
        }
    f.close();
}

bool Logbook::year(uint16_t yr, Year &y)
{
    y = Year();
    y.year = yr;
    uint32_t hours[24] = {};
    {
        File f = LittleFS.open(kDays, "r");
        Day d;
        while (f && f.read((uint8_t *)&d, sizeof(d)) == sizeof(d))
        {
            if (d.date / 10000 != yr) continue;
            ++y.days;
            y.arrivals += d.arrivals;
            y.departures += d.departures;
            const uint16_t moves = d.arrivals + d.departures;
            if (moves > y.bestDay) { y.bestDay = moves; y.bestDayDate = d.date; }
            y.goArounds += d.goArounds;
            if (d.tightestSec && (!y.tightest || d.tightestSec < y.tightest)) { y.tightest = d.tightestSec; y.tightDate = d.date; }
            for (int h = 0; h < 24; ++h) hours[h] += d.hours[h];
        }
        if (f) f.close();
    }
    for (int h = 1; h < 24; ++h)
        if (hours[h] > hours[y.busiestHour]) y.busiestHour = (uint8_t)h;
    const uint16_t from = dayNumber((uint32_t)yr * 10000 + 101), to = dayNumber((uint32_t)yr * 10000 + 1231);
    {
        File f = LittleFS.open(kEvents, "r");
        Event e;
        while (f && f.read((uint8_t *)&e, sizeof(e)) == sizeof(e))
        {
            const time_t t = e.at;
            struct tm lt;
            localtime_r(&t, &lt);
            if (lt.tm_year + 1900 == yr && e.kind == 0) ++y.rares;
        }
        if (f) f.close();
    }
    {
        File f = LittleFS.open(kCollection, "r");
        Item it;
        while (f && f.read((uint8_t *)&it, sizeof(it)) == sizeof(it))
        {
            if (it.firstDay < from || it.firstDay > to) continue;
            if (it.kind == 0) ++y.newAirlines;
            else if (it.kind == 1) ++y.newTypes;
            else if (it.kind == 2) ++y.newAirports;
        }
        if (f) f.close();
    }
    {
        File f = LittleFS.open(Airframes::kPath, "r");
        Airframes::Slot chunk[16];
        size_t got;
        while (f && (got = f.read((uint8_t *)chunk, sizeof(chunk))) >= sizeof(Airframes::Slot))
            for (size_t i = 0; i < got / sizeof(Airframes::Slot); ++i)
            {
                const Airframes::Slot &s = chunk[i];
                if (!s.key || s.lastDay < from || s.firstDay > to) continue;
                if (s.firstDay >= from) ++y.newAirframes;
                if (s.days > y.topDays && s.reg[0])
                {
                    y.topDays = s.days;
                    memcpy(y.topReg, s.reg, 7); y.topReg[7] = 0;
                    memcpy(y.topType, s.type, 4); y.topType[4] = 0;
                }
            }
        if (f) f.close();
    }
    return y.days > 0;
}

void Logbook::recordsJson(String &out)
{
    char a[12] = "", b[12] = "", t[12] = "", s[12] = "";
    if (s_rec.bestDayDate) ymdText(s_rec.bestDayDate, a, sizeof(a));
    if (s_rec.bestGaDate) ymdText(s_rec.bestGaDate, b, sizeof(b));
    if (s_rec.tightDate) ymdText(s_rec.tightDate, t, sizeof(t));
    if (s_rec.since) ymdText(s_rec.since, s, sizeof(s));
    size_t days = 0, events = 0;
    if (LittleFS.exists(kDays)) { File f = LittleFS.open(kDays, "r"); days = f.size() / sizeof(Day); f.close(); }
    if (LittleFS.exists(kEvents)) { File f = LittleFS.open(kEvents, "r"); events = f.size() / sizeof(Event); f.close(); }
    char buf[400];
    snprintf(buf, sizeof(buf),
             "{\"since\":\"%s\",\"days\":%u,\"events\":%u,\"bytes\":%u,\"best_day\":%u,\"best_day_date\":\"%s\","
             "\"best_go_arounds\":%u,\"best_go_arounds_date\":\"%s\",\"tightest\":%u,\"tightest_date\":\"%s\","
             "\"airlines\":%u,\"types\":%u,\"airports\":%u,\"airframes\":%u,\"windiest\":%u}",
             s, (unsigned)days, (unsigned)events, (unsigned)bytesUsed(), s_rec.bestDay, a, s_rec.bestGa, b,
             s_rec.tightest, t, s_rec.airlines, s_rec.types, s_rec.airports, (unsigned)Airframes::count(), s_rec.windiest);
    out = buf;
}
