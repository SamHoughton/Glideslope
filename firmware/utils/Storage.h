#pragma once
/*
Purpose: Never let the flash file system run out (9.2.1).

The framework's LittleFS crashes the board when a write finds no free
block: its "no more free space" message divides by a block count it leaves
at zero (an integer divide by zero, so a panic). Out of space is a restart,
and a restart that tries the same write again is a crash loop: 9.1.4 on
10 November, every minute or so, while the airframe collection rewrote its
192 KB file (utils/Airframes.h).

So every write to LittleFS asks here first, and is skipped (what it was for
waits, or is lost) when it would leave less than a floor free: room for
LittleFS's own housekeeping (a directory's metadata moving to new blocks)
and a margin. Writes the web page asks for (a logo, an airport pack) get a
"storage full" answer instead.

Free space is LittleFS's own count, taken at most every 30 s (it walks the
file system); each write allowed in between is taken off it, counted
generously: its blocks rounded up, plus one for the block a change copies.

Safe mode (utils/Health.h) pauses the background writes; the ones the web
page asks for still go ahead.
*/
#include <Arduino.h>

namespace Storage
{
    constexpr uint32_t kBlock = 4096;
    constexpr uint32_t kFloorBlocks = 24;   // 96 KB never written into
    constexpr uint32_t kLowBlocks = 48;     // under 192 KB free: the Health tab warns
    // Where LittleFS is mounted (LittleFS.begin's default), for POSIX calls.
    constexpr const char *kMount = "/littlefs";

    enum Who : uint8_t { Background, Asked };
    // Room to write `bytes` (new, or a change to a file)? what: for the log,
    // once a minute at most, when refused.
    bool mayWrite(size_t bytes, const char *what, Who who = Background);
    // The rule itself (for the tests): may `bytes` be written with
    // `freeBlocks` free?
    bool allowed(uint32_t freeBlocks, size_t bytes);

    uint32_t freeBlocks();     // counted at most every 30 s
    uint32_t totalBlocks();
    bool     low();            // under kLowBlocks free
    uint32_t refused();        // writes skipped since the start
    void     recount();        // count again on the next ask (after a big change)

    // Safe mode: background writes wait.
    void pause(bool on);
    bool paused();

    // A file or directory there? Its size (-1: none)? Both by stat(): the
    // core's LittleFS.exists() opens the file through stdio, which takes a
    // 4 KB buffer from the heap for every call.
    bool exists(const char *path);
    long size(const char *path);
}
