#pragma once
/*
Purpose: The panel as a picture for phone alerts: a 24-bit BMP at 3x
(384 x 192), each LED drawn as a dot (bright centre, softer edges, dark
corners), generated row by row as a Stream so the 221 KB file is never in
memory: one 1,152-byte row buffer. Reads the frame as it goes (a frame
redrawn mid-send can tear by a row or two; it's a snapshot of a moving
panel).
*/
#include <Arduino.h>
#include "display/FrameCanvas.h"

class FrameBmp : public Stream
{
public:
    static constexpr int    kScale = 3;
    static constexpr int    W = FrameCanvas::W * kScale, H = FrameCanvas::H * kScale;
    static constexpr size_t kRow = W * 3;            // 1,152: a multiple of 4, no padding
    static constexpr size_t kHeader = 54;
    static constexpr size_t kSize = kHeader + kRow * H;

    explicit FrameBmp(const FrameCanvas &f) : _f(f) {}
    ~FrameBmp() override { free(_row); }

    size_t size() const { return kSize; }
    int available() override { return (int)min((size_t)0x7FFF, kSize - _pos); }
    int read() override
    {
        if (_pos >= kSize) return -1;
        const int b = byteAt(_pos);
        if (b >= 0) ++_pos;
        return b;
    }
    size_t readBytes(char *buf, size_t len)   // (virtual on the board, not in the test stub)
    {
        size_t n = 0;
        while (n < len && _pos < kSize)
        {
            const int b = byteAt(_pos);
            if (b < 0) break;
            buf[n++] = (char)b;
            ++_pos;
        }
        return n;
    }
    int peek() override { return _pos < kSize ? byteAt(_pos) : -1; }
    size_t write(uint8_t) override { return 0; }
    void flush() {}

private:
    const FrameCanvas &_f;
    size_t   _pos = 0;
    uint8_t *_row = nullptr;
    int      _rowY = -1;   // file row in _row

    static void le32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

    int byteAt(size_t pos)
    {
        if (pos < kHeader)
        {
            uint8_t h[kHeader] = {'B', 'M'};
            le32(h + 2, kSize);
            le32(h + 10, kHeader);
            le32(h + 14, 40);
            le32(h + 18, W);
            le32(h + 22, H);              // positive: rows bottom-up
            h[26] = 1;                    // planes
            h[28] = 24;                   // bits per pixel
            le32(h + 34, kRow * H);
            le32(h + 38, 2835); le32(h + 42, 2835);   // 72 dpi
            return h[pos];
        }
        const size_t off = pos - kHeader;
        const int y = (int)(off / kRow);
        if (y != _rowY && !fillRow(y)) return -1;
        return _row[off % kRow];
    }

    // File row y (0 = bottom): panel row and the row within its 3x3 dot.
    bool fillRow(int y)
    {
        if (!_row && !(_row = (uint8_t *)malloc(kRow))) return false;
        const int top = H - 1 - y;
        const int py = top / kScale, sy = top % kScale;
        static const uint8_t kDot[3][3] = {{60, 170, 60}, {170, 255, 170}, {60, 170, 60}};
        for (int px = 0; px < FrameCanvas::W; ++px)
        {
            const Rgb c = FrameCanvas::unpack(_f.get(px, py));
            for (int sx = 0; sx < kScale; ++sx)
            {
                const uint8_t k = kDot[sy][sx];
                uint8_t *o = _row + (px * kScale + sx) * 3;
                // A faint unlit LED where the panel is dark, as on the real thing.
                const bool dark = c.r < 8 && c.g < 8 && c.b < 8;
                o[0] = dark ? (uint8_t)(k / 18) : (uint8_t)(c.b * k / 255);   // BMP is BGR
                o[1] = dark ? (uint8_t)(k / 18) : (uint8_t)(c.g * k / 255);
                o[2] = dark ? (uint8_t)(k / 16) : (uint8_t)(c.r * k / 255);
            }
        }
        _rowY = y;
        return true;
    }
};
