#pragma once
/*
Purpose: Installing firmware: an upload from the web page, or (8.2) a
release straight from GitHub at a tap.

Writer streams an image into the spare app slot: it checks the first bytes
are an ESP32-S3 app (not a bootloader or a merged image), hashes the image
for its signature (utils/FirmwareSig.h) and writes it to flash; finish()
refuses a bad signature, and an unsigned build unless the page asked for it.
Anything that fails leaves the running firmware untouched.

installRelease() is the one-tap update: the web page finds the newest
release on GitHub (the browser asks GitHub's API, so the board spends
nothing on it) and asks the board for it by tag; the main loop then
downloads that release's firmware.bin itself,

  https://github.com/SamHoughton/Glideslope/releases/download/<tag>/firmware.bin

(GitHub redirects to its file store), and installs it. Only signed images:
the signature, not the connection, is what makes it genuine.

It does that straight after a restart, before the feed starts: 1.7 MB over
HTTPS wants the memory a fresh boot has (after an hour or two the largest
free block is down to 8-12 KB and a download that long stalls). So the
request restarts the board with the tag kept in RTC memory, setup() finds
it once Wi-Fi is up, and the board restarts again into the new firmware,
or carries on with the reason in /api/status.
*/
#include <Arduino.h>
#include "utils/FirmwareSig.h"

namespace Ota
{
    constexpr const char *kRepo = "SamHoughton/Glideslope";

    class Writer
    {
    public:
        // Each returns nullptr, or why it failed (then call abort()).
        const char *begin(size_t length);
        const char *write(const uint8_t *data, size_t n);   // the first call needs 36 bytes or more
        // Checks the signature and completes the install (restart to run it).
        const char *finish(bool allowUnsigned, bool &signedRelease);
        void        abort();
        size_t      written() const { return _got; }

    private:
        FirmwareSig _sig;
        size_t      _length = 0, _got = 0;
    };
    Writer &writer();   // the one in use (an upload and a download never overlap)

    // One-tap update: the web server asks, the main loop does it.
    // tag: "v8.2.0". False if it isn't a release tag.
    bool request(const char *tag);
    bool takeRequest(char *tag, size_t len);
    // Main loop: restart with the tag kept for setup(); setup(): take it.
    void restartToInstall(const char *tag);
    bool takeBootRequest(char *tag, size_t len);
    // Download and install; restarts on success, else returns why.
    const char *installRelease(const char *tag);
    // The last one-tap update's failure ("" if none), for /api/status.
    const char *lastError();
}
