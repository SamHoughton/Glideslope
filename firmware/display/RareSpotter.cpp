#include "display/RareSpotter.h"
#include "utils/TelnetLogger.h"
#include <LittleFS.h>
#include <set>

namespace
{
    const char *kLogPath = "/types.txt";
    std::set<String> s_seen;
    bool s_loaded = false;

    // Rare types by ICAO designator (prefix match where marked).
    struct Rule { const char *code; bool prefix; const char *name; };
    const Rule kRareTypes[] = {
        {"A388", false, "A380"}, {"A389", false, "A380"},
        {"B74",  true,  "747"},
        {"A124", false, "AN-124"}, {"A225", false, "AN-225"},
        {"CONC", false, "CONCORDE"},
    };
    // Military types (any operator).
    const char *const kMilitaryTypes[] = {
        "C17", "A400", "C130", "C30J", "K35R", "KC2", "P8", "E3TF", "E3CF", "A3ST",
        "EUFI", "F35", "TYPH", "C5M", "VOYG", "B52", "HAWK",
    };
    // Military call-sign prefixes: RAF, USAF AMC, Luftwaffe, French AF, Belgian AF,
    // Canadian forces, Italian AF, NATO, Spanish AF, Netherlands AF.
    const char *const kMilitaryCallsigns[] = {
        "RRR", "RCH", "GAF", "CTM", "BAF", "CFC", "IAM", "NATO", "AME", "NAF", "ASCOT",
    };

    String upperType(const FlightInfo &f)
    {
        String t = f.aircraft_code;
        t.trim();
        t.toUpperCase();
        return t;
    }
}

void RareSpotter::begin()
{
    s_seen.clear();
    File f = LittleFS.open(kLogPath, "r");
    if (f)
    {
        while (f.available())
        {
            String line = f.readStringUntil('\n');
            line.trim();
            if (line.length()) s_seen.insert(line);
        }
        f.close();
    }
    s_loaded = true;
    Log.printf("RareSpotter: %u aircraft types seen so far\n", (unsigned)s_seen.size());
}

RareSpotter::Reason RareSpotter::check(const FlightInfo &f)
{
    const String type = upperType(f);
    String cs = f.ident;
    cs.toUpperCase();

    Reason r = None;
    for (const char *p : kMilitaryCallsigns)
        if (cs.startsWith(p)) { r = Military; break; }
    if (r == None && type.length())
        for (const char *t : kMilitaryTypes)
            if (type == t) { r = Military; break; }
    if (r == None && type.length())
        for (const Rule &rule : kRareTypes)
            if (rule.prefix ? type.startsWith(rule.code) : type == rule.code) { r = Rare; break; }

    // First sighting: log the type (only once the log has loaded, so a
    // failed mount doesn't make everything "first").
    if (type.length() && s_loaded && s_seen.find(type) == s_seen.end())
    {
        s_seen.insert(type);
        File out = LittleFS.open(kLogPath, "a");
        if (out) { out.println(type); out.close(); }
        if (r == None) r = FirstSeen;
        Log.printf("RareSpotter: first sighting of %s\n", type.c_str());
    }
    return r;
}

void RareSpotter::banner(Reason r, const FlightInfo &f, char *line1, size_t len1, char *line2, size_t len2)
{
    const String type = upperType(f);
    const char *pretty = type.c_str();
    for (const Rule &rule : kRareTypes)
        if (rule.prefix ? type.startsWith(rule.code) : type == rule.code) { pretty = rule.name; break; }

    switch (r)
    {
        case Rare:      snprintf(line1, len1, "RARE SPOT");      snprintf(line2, len2, "%s", pretty); break;
        case Military:  snprintf(line1, len1, "MILITARY");       snprintf(line2, len2, "%s", type.length() ? pretty : f.ident.c_str()); break;
        case FirstSeen: snprintf(line1, len1, "FIRST SIGHTING"); snprintf(line2, len2, "%s", pretty); break;
        default:        line1[0] = line2[0] = '\0'; break;
    }
}
