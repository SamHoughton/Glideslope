#pragma once
/*
Purpose: Read an HTTP/1.1 chunked body as plain bytes, so a reply can be
parsed straight from the connection (ReadsbParser::parseStream) while the
connection stays open for the next request. adsb.fi answers HTTP/1.1
requests chunked: "<hex size>\r\n<data>\r\n" ... "0\r\n\r\n".

finish() reads to the end of the body (the last chunk and the blank line
after it), so the next request on the connection starts clean.
*/
#include <Arduino.h>

class ChunkedStream : public Stream
{
public:
    explicit ChunkedStream(Stream &in, unsigned long timeoutMs = 4000) : _in(in), _timeout(timeoutMs) {}

    int available() override
    {
        if (!fill()) return 0;
        const int a = _in.available();
        return a < 0 ? 0 : (int)min((size_t)a, _left);
    }
    int read() override
    {
        if (!fill()) return -1;
        const int c = waitByte();
        if (c >= 0) --_left;
        return c;
    }
    int peek() override
    {
        if (!fill()) return -1;
        if (!wait()) return -1;
        return _in.peek();
    }
    size_t write(uint8_t) override { return 0; }
    void flush() override {}

    // Read the rest of the body, last chunk included.
    void finish() { while (read() >= 0) {} }
    bool done() const { return _done; }

private:
    Stream       &_in;
    unsigned long _timeout;
    size_t        _left = 0;
    bool          _started = false, _done = false;

    bool wait()
    {
        const unsigned long t0 = millis();
        while (!_in.available())
        {
            if (millis() - t0 > _timeout) return false;
            delay(1);
        }
        return true;
    }
    int waitByte() { return wait() ? _in.read() : -1; }

    // One line ("\r\n" ended) into buf; false on timeout.
    bool line(char *buf, size_t len)
    {
        size_t n = 0;
        for (;;)
        {
            const int c = waitByte();
            if (c < 0) return false;
            if (c == '\n') break;
            if (c != '\r' && n + 1 < len) buf[n++] = (char)c;
        }
        buf[n] = '\0';
        return true;
    }

    // Make sure there is body left in the current chunk, reading the next
    // chunk's header if need be. False at the end of the body.
    bool fill()
    {
        if (_done) return false;
        if (_left) return true;
        char buf[24];
        if (_started && !line(buf, sizeof(buf))) { _done = true; return false; }   // CRLF after the data
        _started = true;
        if (!line(buf, sizeof(buf))) { _done = true; return false; }
        _left = strtoul(buf, nullptr, 16);   // stops at ';' (extensions)
        if (_left == 0)
        {
            while (line(buf, sizeof(buf)) && buf[0]) {}   // trailers, then the blank line
            _done = true;
            return false;
        }
        return true;
    }
};
