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
{
    memset(_ring, 0, sizeof(_ring));
}

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

void TelnetLogger::pushLine()
{
    // The stored line carries its timestamp so the web log panel shows it too.
    char *slot = _ring[_seqNext % MAX_LINES];
    makeTimestamp(slot, 12);
    const size_t ts = strlen(slot);
    const size_t n  = min(_pendingLen, LINE_LEN - 1 - ts);
    memcpy(slot + ts, _pending, n);
    slot[ts + n] = '\0';
    ++_seqNext;
    _pendingLen = 0;
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
    }

    // Serial is best effort: skip it when the USB buffer is full (nothing on
    // the PC reading), so logging can never stall the caller.
    if (Serial.availableForWrite() > 0) Serial.write(c);

    if (c == '\n')
    {
        pushLine();
        _lineStart = true;
    }
    else if (c != '\r' && _pendingLen < LINE_LEN - 1)
    {
        _pending[_pendingLen++] = (char)c;
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

size_t TelnetLogger::linesJson(uint32_t cursor, size_t maxLines, char *buf, size_t len) const
{
    if (len < 64) return 0;
    if (_mutex && xSemaphoreTake(_mutex, kLockTicks) != pdTRUE) return 0;

    // Oldest line still held, then at most maxLines of the newest.
    const uint32_t oldest = _seqNext > MAX_LINES ? _seqNext - MAX_LINES : 0;
    uint32_t seq = cursor < oldest ? oldest : cursor;
    if (seq > _seqNext) seq = oldest;                       // cursor from before a restart
    if (_seqNext - seq > maxLines) seq = _seqNext - maxLines;

    // Leave room for the closing "]}" and the cursor; a line that won't fit
    // ends the batch and the browser fetches the rest next time.
    size_t n = 0;
    const size_t limit = len - 32;
    buf[n++] = '['; 
    bool first = true;
    for (; seq < _seqNext; ++seq)
    {
        const char *line = _ring[seq % MAX_LINES];
        const size_t need = strlen(line) * 2 + 3;            // worst case: every char escaped
        if (n + need > limit) break;
        if (!first) buf[n++] = ',';
        first = false;
        buf[n++] = '"';
        for (const char *p = line; *p; ++p)
        {
            const char ch = *p;
            if (ch == '"' || ch == '\\') { buf[n++] = '\\'; buf[n++] = ch; }
            else if ((unsigned char)ch >= 0x20) buf[n++] = ch;
        }
        buf[n++] = '"';
    }
    if (_mutex) xSemaphoreGive(_mutex);

    // Prefix the cursor: shift the lines right to make room.
    char head[32];
    const int h = snprintf(head, sizeof(head), "{\"cursor\":%lu,\"lines\":", (unsigned long)seq);
    memmove(buf + h, buf, n);
    memcpy(buf, head, h);
    n += h;
    buf[n++] = ']';
    buf[n++] = '}';
    buf[n] = '\0';
    return n;
}
