#pragma once

/*
Purpose: Lightweight log capture — writes to Serial and keeps the recent log
lines that the web UI streams via /api/log.

Memory: the lines live in a fixed ring of char arrays (part of the global
object, so never on the heap). An earlier String-per-line deque, with each
line grown a character at a time, fragmented the heap until TLS handshakes
could no longer find a large enough block.

Usage:
  Log.print() / Log.println() / Log.printf() — identical call surface to Serial.
*/

#include <Arduino.h>
#include <freertos/semphr.h>

class TelnetLogger : public Print
{
public:
    TelnetLogger();

    static constexpr size_t MAX_LINES = 100;   // retained lines
    static constexpr size_t LINE_LEN  = 120;   // per line, timestamp included; longer lines are cut

    // Print overrides — write to Serial AND the ring.
    size_t write(uint8_t c) override;
    size_t write(const uint8_t *buf, size_t size) override;
    using Print::write;

    // Lines from sequence number `cursor` on (at most maxLines, the newest),
    // as the JSON {"cursor":N,"lines":["...",...]} written into buf.
    // Returns the JSON length (0 if it could not be built).
    // Thread-safe: may be called from a different task than write().
    size_t linesJson(uint32_t cursor, size_t maxLines, char *buf, size_t len) const;

private:
    char     _ring[MAX_LINES][LINE_LEN];
    char     _pending[LINE_LEN];      // the line being written (after its timestamp)
    size_t   _pendingLen = 0;
    uint32_t _seqNext = 0;            // sequence number of the next completed line

    mutable SemaphoreHandle_t _mutex = nullptr;
    bool _lineStart = true;           // true when the next byte begins a new line

    // Internal helpers — must be called with _mutex held.
    void pushLine();
    void writeByte(uint8_t c);
};

// Global singleton — include this header and call Log.printf() etc.
extern TelnetLogger Log;
