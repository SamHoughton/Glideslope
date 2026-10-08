#include "config/Airport.h"
#include "display/MapBase.h"
#include "utils/TelnetLogger.h"
#include <ArduinoJson.h>
#include <LittleFS.h>

Airport g_airport;

namespace
{
    uint8_t *s_map = nullptr;   // pack map (4-bit indices); nullptr = built-in London map

    void heathrow(Airport &a)
    {
        a = Airport();
        strlcpy(a.icao, "EGLL", sizeof(a.icao));
        strlcpy(a.iata, "LHR", sizeof(a.iata));
        strlcpy(a.name, "HEATHROW", sizeof(a.name));
        a.lat = 51.4700;  a.lon = -0.4543;
        strlcpy(a.tz, "GMT0BST,M3.5.0/1,M10.5.0", sizeof(a.tz));
        // Landing thresholds, within ~100 m: good enough to pick a runway and
        // estimate time to touchdown.
        static const RunwayEnd kRunways[] = {
            {"27R", 51.4777, -0.4332, 269.7f},   // north runway, east end, landing westbound
            {"27L", 51.4649, -0.4340, 269.7f},   // south runway, east end
            {"09L", 51.4775, -0.4850,  89.7f},   // north runway, west end, landing eastbound
            {"09R", 51.4647, -0.4826,  89.7f},   // south runway, west end
        };
        for (const RunwayEnd &r : kRunways) a.runways[a.runwayCount++] = r;
        // The four arrival stacks (VOR holding fixes).
        static const HoldFix kHolds[] = {
            {"BNN", 51.7262, -0.5500},   // Bovingdon, north-west
            {"LAM", 51.6461,  0.1517},   // Lambourne, north-east
            {"BIG", 51.3311,  0.0347},   // Biggin Hill, south-east
            {"OCK", 51.3050, -0.4472},   // Ockham, south-west
        };
        for (const HoldFix &h : kHolds) a.holds[a.holdCount++] = h;
        a.lhrAlternation = true;
        a.londonSkyline  = true;
        a.mapN = MapBase::LAT_N;  a.mapS = MapBase::LAT_S;
        a.mapW = MapBase::LON_W;  a.mapE = MapBase::LON_E;
        a.builtIn = true;
    }

    void copyUpper(char *dst, size_t len, const char *src)
    {
        strlcpy(dst, src ? src : "", len);
        for (char *p = dst; *p; ++p) *p = toupper((unsigned char)*p);
    }
}

bool AirportPack::parseHeader(const char *json, Airport &a, String &error)
{
    JsonDocument doc;
    if (deserializeJson(doc, json)) { error = "header is not JSON"; return false; }
    if ((doc["v"] | 0) != 1)        { error = "unknown pack version"; return false; }

    a = Airport();
    copyUpper(a.icao, sizeof(a.icao), doc["icao"] | "");
    copyUpper(a.iata, sizeof(a.iata), doc["iata"] | "");
    copyUpper(a.name, sizeof(a.name), doc["name"] | "");
    a.lat = doc["lat"] | NAN;
    a.lon = doc["lon"] | NAN;
    strlcpy(a.tz, doc["tz"] | "UTC0", sizeof(a.tz));
    a.lhrAlternation = doc["alternation"] | false;
    a.londonSkyline  = strcmp(doc["skyline"] | "generic", "london") == 0;
    a.mapN = doc["map"]["n"] | NAN;  a.mapS = doc["map"]["s"] | NAN;
    a.mapW = doc["map"]["w"] | NAN;  a.mapE = doc["map"]["e"] | NAN;
    for (JsonObject r : doc["runways"].as<JsonArray>())
    {
        if (a.runwayCount >= Airport::kMaxRunways) break;
        RunwayEnd &e = a.runways[a.runwayCount];
        copyUpper(e.name, sizeof(e.name), r["n"] | "");
        e.lat = r["lat"] | NAN;  e.lon = r["lon"] | NAN;  e.course = r["crs"] | NAN;
        if (e.name[0] && !isnan(e.lat) && !isnan(e.lon) && !isnan(e.course)) ++a.runwayCount;
    }
    for (JsonObject h : doc["holds"].as<JsonArray>())
    {
        if (a.holdCount >= Airport::kMaxHolds) break;
        HoldFix &f = a.holds[a.holdCount];
        copyUpper(f.name, sizeof(f.name), h["n"] | "");
        f.lat = h["lat"] | NAN;  f.lon = h["lon"] | NAN;
        if (f.name[0] && !isnan(f.lat) && !isnan(f.lon)) ++a.holdCount;
    }
    a.builtIn = false;

    if (strlen(a.icao) != 4)                         { error = "missing ICAO code"; return false; }
    if (isnan(a.lat) || isnan(a.lon))                { error = "missing airport position"; return false; }
    if (a.runwayCount == 0)                          { error = "no runways"; return false; }
    if (!(a.mapN > a.mapS) || !(a.mapE > a.mapW))    { error = "bad map bounds"; return false; }
    if (!a.name[0]) strlcpy(a.name, a.icao, sizeof(a.name));
    return true;
}

void AirportPack::load()
{
    heathrow(g_airport);
    File f = LittleFS.open(kPath, "r");
    if (!f) { Log.println("Airport: Heathrow (built in)"); return; }

    String header = f.readStringUntil('\n');
    Airport a;
    String error;
    uint8_t *map = (uint8_t *)malloc(kMapBytes);
    const bool ok = map && parseHeader(header.c_str(), a, error) &&
                    f.read(map, kMapBytes) == kMapBytes;
    f.close();
    if (!ok)
    {
        free(map);
        Log.printf("Airport: pack unreadable (%s), using Heathrow\n", error.length() ? error.c_str() : "short file");
        return;
    }
    g_airport = a;
    s_map = map;
    Log.printf("Airport: %s (%s), %u runway ends, from %s\n", g_airport.name, g_airport.icao,
               (unsigned)g_airport.runwayCount, kPath);
}

uint8_t AirportPack::mapIndex(int x, int y)
{
    if ((unsigned)x >= 128 || (unsigned)y >= 64) return 0;
    if (!s_map) return MapBase::kRows[y][x] - '0';
    const uint8_t b = s_map[(y * 128 + x) / 2];
    return (x & 1) ? (b & 0x0F) : (b >> 4);
}

const uint8_t *AirportPack::mapColour(uint8_t index)
{
    return MapBase::kPalette[index < 8 ? index : 0];
}

bool AirportPack::isHome(const AirportInfo &a)
{
    return a.code_icao == g_airport.icao || (g_airport.iata[0] && a.code_iata == g_airport.iata);
}
