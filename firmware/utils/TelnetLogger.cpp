/*
Purpose: TelnetLogger — Serial mirror + ring-buffer for the web log panel.
Lines are accumulated character-by-character until '\n', then pushed into the
deque.  The deque is capped at MAX_LINES; the oldest line is evicted when full.
*/
#include "utils/TelnetLogger.h"
#include "config/RuntimeConfig.h"
#include <time.h>

TelnetLogger Log;

// ---------------------------------------------------------------------------

TelnetLogger::TelnetLogger()
    : _mutex(xSemaphoreCreateMutex())
{}

// ---------------------------------------------------------------------------
// Timestamp helper
// ---------------------------------------------------------------------------

// Writes "[HH:MM:SS] " (local time) into buf (must be ≥ 12 bytes).
// Falls back to "[--:--:--] " if NTP has not yet synced.
static void makeTimestamp(char *buf, size_t bufLen)
{
    time_t utcNow = time(nullptr);
    if (utcNow < 946684800L)   // pre-year-2000 → NTP not yet synced
    {
        strncpy(buf, "[--:--:--] ", bufLen);
        buf[bufLen - 1] = '\0';
        return;
    }
    // UK local time (GMT/BST) via the TZ rule set at boot.
    struct tm tmBuf;
    if (!localtime_r(&utcNow, &tmBuf))
    {
        strncpy(buf, "[--:--:--] ", bufLen);
        buf[bufLen - 1] = '\0';
        return;
    }
    snprintf(buf, bufLen, "[%02d:%02d:%02d] ",
             tmBuf.tm_hour, tmBuf.tm_min, tmBuf.tm_sec);
}

// ---------------------------------------------------------------------------
// Private helpers (always called with _mutex held)
// ---------------------------------------------------------------------------

void TelnetLogger::pushLine(const String &line)
{
    // Prepend timestamp to the stored line so the web log panel shows it too
    char ts[12];
    makeTimestamp(ts, sizeof(ts));
    _lines.push_back(String(ts) + line);
    _seqNext++;
    while (_lines.size() > MAX_LINES)
    {
        _lines.pop_front();
        _seqBase++;
    }
}

// Core single-byte write — handles timestamp injection and line accumulation.
void TelnetLogger::writeByte(uint8_t c)
{
    if (_lineStart && c != '\n')
    {
        // First real character of a new line: emit timestamp on Serial
        char ts[12];
        makeTimestamp(ts, sizeof(ts));
        if (Serial.availableForWrite() >= (int)strlen(ts)) Serial.print(ts);
        _lineStart = false;
        // Note: timestamp is NOT added to _pending — pushLine() prepends it
        // to the stored line so we don't double-store it in memory.
    }

    // Serial is best effort: skip it when the USB buffer is full (nothing on
    // the PC reading), so logging can never stall the caller.
    if (Serial.availableForWrite() > 0) Serial.write(c);

    if (c == '\n')
    {
        pushLine(_pending);
        _pending = "";
        _lineStart = true;
    }
    else
    {
        _pending += (char)c;
    }
}

// ---------------------------------------------------------------------------
// Public Print overrides
// ---------------------------------------------------------------------------

// Never wait on the log lock for long: losing a log line (or one batch for the
// web log panel) is better than a task stuck behind it.
static constexpr TickType_t kLockTicks = pdMS_TO_TICKS(200);

size_t TelnetLogger::write(uint8_t c)
{
    if (_mutex && xSemaphoreTake(_mutex, kLockTicks) != pdTRUE) return 0;
    writeByte(c);
    if (_mutex) xSemaphoreGive(_mutex);
    return 1;
}

size_t TelnetLogger::write(const uint8_t *buf, size_t size)
{
    // Process byte-by-byte so timestamp injection works at every line boundary.
    if (_mutex && xSemaphoreTake(_mutex, kLockTicks) != pdTRUE) return 0;
    for (size_t i = 0; i < size; ++i)
        writeByte(buf[i]);
    if (_mutex) xSemaphoreGive(_mutex);
    return size;
}

void TelnetLogger::getLines(uint32_t cursor,
                            std::vector<String> &out,
                            uint32_t &nextCursor) const
{
    out.clear();
    nextCursor = cursor;   // unchanged if the lock is busy: the caller just retries
    if (_mutex && xSemaphoreTake(_mutex, kLockTicks) != pdTRUE) return;

    nextCursor = _seqNext;

    // Clamp cursor to the oldest line we still have
    uint32_t start = (cursor < _seqBase) ? _seqBase : cursor;
    if (start < _seqNext)
    {
        size_t idx = (size_t)(start - _seqBase);
        for (size_t i = idx; i < _lines.size(); ++i)
            out.push_back(_lines[i]);
    }

    if (_mutex) xSemaphoreGive(_mutex);
}
