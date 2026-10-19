#pragma once
/*
Purpose: Roll back a bad firmware update by itself. The bootloader supports
it (CONFIG_APP_ROLLBACK_ENABLE); the Arduino core just marks every new image
good as it starts. Here a new image stays "pending" until it has proved
itself: a successful position fetch, or five minutes up. If it crashes or
hangs (watchdog) before that, the next boot goes back to the previous image.
A deliberate restart (installing an airport pack, the Restart button) keeps
the image first, so it never counts as a failure.
*/
#include <Arduino.h>

namespace ImageCheck
{
    // Log whether this image is new and still on trial. Call early in setup().
    void begin();
    // Keep the running image (no-op once kept). why: for the log.
    void keep(const char *why);
    // "on trial", "kept" or "factory" (USB-flashed, no rollback possible).
    const char *state();
}
