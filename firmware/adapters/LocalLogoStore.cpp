/*
Purpose: Load pre-converted airline logo bitmaps from LittleFS.
Responsibilities:
- Mount LittleFS once at startup.
- Open /logos/{ICAO}.bin, validate exact expected byte count, read into vector.
- Return false silently for missing airlines.
*/
#include "adapters/LocalLogoStore.h"
#include "models/FlightInfo.h"
#include <LittleFS.h>
#include "utils/TelnetLogger.h"
#include <algorithm>

bool LocalLogoStore::initialize()
{
    if (!LittleFS.begin(false))
    {
        Log.println("LocalLogoStore: LittleFS mount failed. "
                    "Run 'pio run --target uploadfs' to upload logo data.");
        _mounted = false;
        return false;
    }
    _mounted = true;
    Log.println("LocalLogoStore: LittleFS mounted OK.");

    size_t total = LittleFS.totalBytes();
    size_t used  = LittleFS.usedBytes();
    Log.printf("LocalLogoStore: %u bytes total, %u used (%.1f%%)\n",
               (unsigned)total, (unsigned)used,
               total > 0 ? (used * 100.0f / total) : 0.0f);
    return true;
}

bool LocalLogoStore::hasAirlineLogo(const String &airlineIcao)
{
    if (!_mounted || airlineIcao.length() == 0) return false;
    String icao = airlineIcao;
    icao.toUpperCase();
    return LittleFS.exists(String("/logos/") + icao + ".bin");
}

bool LocalLogoStore::getAirlineLogo(const String &airlineIcao,
                                    std::vector<uint16_t> &outPixels)
{
    outPixels.clear();

    if (!_mounted || airlineIcao.length() == 0)
        return false;

    String icao = airlineIcao;
    icao.toUpperCase();

    String path = String("/logos/") + icao + ".bin";

    if (!LittleFS.exists(path))
    {
        Log.printf("LocalLogoStore: No logo for %s\n", icao.c_str());
        return false;
    }

    File f = LittleFS.open(path, "r");
    if (!f)
    {
        Log.printf("LocalLogoStore: Failed to open %s\n", path.c_str());
        return false;
    }

    const size_t expectedBytes = (size_t)AirlineLogo::WIDTH * AirlineLogo::HEIGHT * 2;

    if (f.size() != expectedBytes)
    {
        Log.printf("LocalLogoStore: Size mismatch for %s: got %u, expected %u\n",
                   path.c_str(), (unsigned)f.size(), (unsigned)expectedBytes);
        f.close();
        return false;
    }

    const size_t pixelCount = (size_t)AirlineLogo::WIDTH * AirlineLogo::HEIGHT;
    outPixels.resize(pixelCount);

    size_t bytesRead = f.read(reinterpret_cast<uint8_t *>(outPixels.data()), expectedBytes);
    f.close();

    if (bytesRead != expectedBytes)
    {
        Log.printf("LocalLogoStore: Short read for %s: got %u bytes\n",
                   path.c_str(), (unsigned)bytesRead);
        outPixels.clear();
        return false;
    }

    // ESP32 is little-endian and files are written as little-endian uint16_t —
    // the in-place cast above is already correct, no byte-swapping needed.
    Log.printf("LocalLogoStore: Loaded logo for %s (%u pixels)\n",
               icao.c_str(), (unsigned)pixelCount);
    return true;
}

// ── LogoBook ─────────────────────────────────────────────────────────────────

namespace LogoBook
{
namespace
{
struct Missing { char code[5]; uint16_t seen; };
Missing           s_missing[16] = {};
portMUX_TYPE      s_mux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t s_version = 1;

bool validCode(const String &c)
{
    if (c.length() < 2 || c.length() > 4) return false;
    for (unsigned i = 0; i < c.length(); ++i)
        if (!isupper((unsigned char)c[i]) && !isdigit((unsigned char)c[i])) return false;
    return true;
}
}

void noteMissing(const String &icao)
{
    if (!validCode(icao)) return;
    portENTER_CRITICAL(&s_mux);
    Missing *slot = nullptr, *least = &s_missing[0];
    for (Missing &m : s_missing)
    {
        if (!strncmp(m.code, icao.c_str(), sizeof(m.code))) { slot = &m; break; }
        if (m.seen < least->seen) least = &m;
    }
    if (!slot)
    {
        slot = least;   // an empty slot (seen 0) or the least seen
        strlcpy(slot->code, icao.c_str(), sizeof(slot->code));
        slot->seen = 0;
    }
    if (slot->seen < 0xFFFF) ++slot->seen;
    portEXIT_CRITICAL(&s_mux);
}

String missingJson()
{
    Missing copy[16];
    portENTER_CRITICAL(&s_mux);
    memcpy(copy, s_missing, sizeof(copy));
    portEXIT_CRITICAL(&s_mux);
    std::sort(copy, copy + 16, [](const Missing &a, const Missing &b) { return a.seen > b.seen; });
    String out = "[";
    for (const Missing &m : copy)
    {
        if (!m.seen || !m.code[0]) continue;
        if (LittleFS.exists(String("/logos/") + m.code + ".bin")) continue;   // added since
        if (out.length() > 1) out += ",";
        out += String("{\"code\":\"") + m.code + "\",\"seen\":" + String(m.seen) + "}";
    }
    return out + "]";
}

bool save(const String &code, const uint8_t *data, size_t len)
{
    if (!validCode(code) || len != kBytes) return false;
    const String path = String("/logos/") + code + ".bin", tmp = "/logos/upload.tmp";
    if (!LittleFS.exists("/logos")) LittleFS.mkdir("/logos");
    File f = LittleFS.open(tmp, "w");
    if (!f) return false;
    const bool ok = f.write(data, len) == len;
    f.close();
    if (!ok) { LittleFS.remove(tmp); return false; }
    LittleFS.remove(path);
    if (!LittleFS.rename(tmp, path)) return false;
    ++s_version;
    Log.printf("Logos: added %s\n", code.c_str());
    return true;
}

bool remove(const String &code)
{
    if (!validCode(code)) return false;
    const bool ok = LittleFS.remove(String("/logos/") + code + ".bin");
    if (ok) ++s_version;
    return ok;
}

int count()
{
    File dir = LittleFS.open("/logos");
    if (!dir || !dir.isDirectory()) return 0;
    int n = 0;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile())
        if (String(f.name()).endsWith(".bin")) ++n;
    return n;
}

uint32_t version() { return s_version; }
}
