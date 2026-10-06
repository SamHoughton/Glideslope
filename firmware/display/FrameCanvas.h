#pragma once
/*
Purpose: Off-screen 128x64 RGB565 frame that cards and animations draw into.
The display pushes a finished frame to the HUB75 panel, sending only the
pixels that differ from what is already on screen.
Memory: one frame is 16 KB; buffers are static, never on the heap.
*/
#include <Arduino.h>

struct Rgb { uint8_t r, g, b; };

class FrameCanvas
{
public:
    static constexpr int W = 128;
    static constexpr int H = 64;

    static uint16_t pack(Rgb c)
    {
        return ((c.r & 0xF8) << 8) | ((c.g & 0xFC) << 3) | (c.b >> 3);
    }
    static Rgb unpack(uint16_t v)
    {
        uint8_t r = (v >> 11) & 0x1F, g = (v >> 5) & 0x3F, b = v & 0x1F;
        return { (uint8_t)((r << 3) | (r >> 2)), (uint8_t)((g << 2) | (g >> 4)), (uint8_t)((b << 3) | (b >> 2)) };
    }
    static Rgb scale(Rgb c, float k)
    {
        if (k < 0) k = 0;
        if (k > 1) k = 1;
        return { (uint8_t)(c.r * k), (uint8_t)(c.g * k), (uint8_t)(c.b * k) };
    }

    void clear() { memset(_px, 0, sizeof(_px)); }
    void copyFrom(const FrameCanvas &o) { memcpy(_px, o._px, sizeof(_px)); }

    void set(int x, int y, uint16_t v)
    {
        if (x >= _clipX0 && x <= _clipX1 && (unsigned)y < (unsigned)H) _px[y * W + x] = v;
    }
    // Restrict drawing to columns x0..x1 (e.g. scrolling text); resetClip() undoes it.
    void setClip(int x0, int x1) { _clipX0 = x0 < 0 ? 0 : x0; _clipX1 = x1 > W - 1 ? W - 1 : x1; }
    void resetClip() { _clipX0 = 0; _clipX1 = W - 1; }
    void set(int x, int y, Rgb c) { set(x, y, pack(c)); }
    uint16_t get(int x, int y) const
    {
        return ((unsigned)x < (unsigned)W && (unsigned)y < (unsigned)H) ? _px[y * W + x] : 0;
    }

    void fillRect(int x, int y, int w, int h, Rgb c);

    // 5x7 text. scale multiplies each font pixel; pitch is the advance per
    // character in output pixels (0 = default: 6 * scale); maxChars truncates
    // (-1 = whole string). Takes plain C strings so drawing never allocates.
    void text(int x, int y, const char *s, Rgb c, int scale = 1, int pitch = 0, int maxChars = -1);
    static int textWidth(const char *s, int scale = 1, int pitch = 0, int maxChars = -1);
    void text(int x, int y, const String &s, Rgb c, int scale = 1, int pitch = 0, int maxChars = -1)
    {
        text(x, y, s.c_str(), c, scale, pitch, maxChars);
    }
    static int textWidth(const String &s, int scale = 1, int pitch = 0, int maxChars = -1)
    {
        return textWidth(s.c_str(), scale, pitch, maxChars);
    }

    const uint16_t *pixels() const { return _px; }
    uint16_t *pixels() { return _px; }

private:
    uint16_t _px[W * H];
    int      _clipX0 = 0, _clipX1 = W - 1;
};
