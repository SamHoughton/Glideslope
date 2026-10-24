#include "utils/Logbook.h"
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

    const char *const kKinds[] = {"rare", "first visit", "watchlist", "overhead", "go-around", "emergency"};
}

void Logbook::begin()
{
    if (!LittleFS.exists(kDir)) LittleFS.mkdir(kDir);
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
        snprintf(line, sizeof(line), "%s,%s,%s,%s\n", e.at ? when : "", e.kind < 6 ? kKinds[e.kind] : "other", id, what);
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
        snprintf(line, sizeof(line), "%s,%s,%s,%s,%u,%lu\n", it.kind ? "type" : "airline", code, first, last, it.days,
                 (unsigned long)it.count);
        w.line(line);
    }
    f.close();
}

size_t Logbook::bytesUsed()
{
    size_t n = 0;
    for (const char *p : {kDays, kEvents, kCollection, kRecords})
        if (LittleFS.exists(p)) { File f = LittleFS.open(p, "r"); n += f.size(); f.close(); }
    return n;
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
             "\"airlines\":%u,\"types\":%u}",
             s, (unsigned)days, (unsigned)events, (unsigned)bytesUsed(), s_rec.bestDay, a, s_rec.bestGa, b,
             s_rec.tightest, t, s_rec.airlines, s_rec.types);
    out = buf;
}
