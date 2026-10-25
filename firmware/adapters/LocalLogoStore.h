#pragma once
#include "display/FrameCanvas.h"   // Rgb
/*
Purpose: Load pre-converted airline logo bitmaps from LittleFS.
File format: /logos/{ICAO_UPPERCASE}.bin — AirlineLogo::WIDTH * HEIGHT * 2 bytes,
             little-endian RGB565 pixels, row-major. Magenta (0xF81F) = transparent.
Populate: run 'pio run --target uploadfs' after running tools/build_logos.py.
*/
#include "interfaces/BaseLogoStore.h"

class LocalLogoStore : public BaseLogoStore
{
public:
    bool initialize();

    // The logo's accent colour, read from its file 64 pixels at a time:
    // nothing on the heap (8.3.1). False if there is no logo.
    bool accentOf(const char *icao3, Rgb &out);
    bool getAirlineLogo(const String &airlineIcao,
                        std::vector<uint16_t> &outPixels) override;
    bool hasAirlineLogo(const String &airlineIcao) override;

private:
    bool _mounted = false;
};

// The logos on the board, from the web page's side: which airlines were seen
// without one (for the Logos section), and adding one (POST /api/logo).
// Safe from any task.
namespace LogoBook
{
    static constexpr size_t kBytes = 32 * 32 * 2;   // one logo: 32x32 RGB565

    // An airline seen in traffic with no logo file (ICAO code).
    void noteMissing(const String &icao);
    // Kept in flash (/logos/missing.bin) so the list builds up across
    // restarts (8.1): load at start, save from the main loop when changed.
    void loadMissing();
    void saveMissingIfChanged();
    // [{"code":"CTN","seen":12},...], most seen first, without any added since.
    String missingJson();
    // Write /logos/<CODE>.bin (via a temporary file); false on a bad code or size.
    bool save(const String &code, const uint8_t *data, size_t len);
    bool remove(const String &code);
    int count();
    // Changes whenever a logo is added: callers caching "no logo" start again.
    uint32_t version();
}
