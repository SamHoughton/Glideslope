#pragma once
/*
Purpose: The frame a transition starts from (cross-fade, fly-across), kept
at 12 bits a pixel (4:4:4, two pixels in three bytes) instead of 16. It is
only ever on the panel while it fades or is wiped away, and at 4:4:4 it
looks the same as the full frame; 8 bits (3:3:2) visibly greyed dusk skies.
Memory: 12 KB instead of 16 KB, static.
*/
#include "FrameCanvas.h"

class PackedFrame
{
public:
    static constexpr int N = FrameCanvas::W * FrameCanvas::H;

    // RGB565 -> 4:4:4 and back.
    static uint16_t to444(uint16_t v)
    {
        return (uint16_t)(((v >> 12) & 0xF) << 8 | ((v >> 7) & 0xF) << 4 | ((v >> 1) & 0xF));
    }
    static uint16_t to565(uint16_t q)
    {
        const uint16_t r = (q >> 8) & 0xF, g = (q >> 4) & 0xF, b = q & 0xF;
        return (uint16_t)(((r << 1) | (r >> 3)) << 11 | ((g << 2) | (g >> 2)) << 5 | ((b << 1) | (b >> 3)));
    }

    void clear() { memset(_b, 0, sizeof(_b)); }
    void copyFrom(const FrameCanvas &f)
    {
        const uint16_t *px = f.pixels();
        for (int i = 0, o = 0; i < N; i += 2, o += 3)
        {
            const uint16_t a = to444(px[i]), b = to444(px[i + 1]);
            _b[o]     = (uint8_t)a;
            _b[o + 1] = (uint8_t)((a >> 8) | (b << 4));
            _b[o + 2] = (uint8_t)(b >> 4);
        }
    }
    // Pixel i at 4:4:4.
    uint16_t raw(int i) const
    {
        const int o = (i >> 1) * 3;
        return (i & 1) ? (uint16_t)((_b[o + 1] >> 4) | (_b[o + 2] << 4))
                       : (uint16_t)(_b[o] | ((_b[o + 1] & 0xF) << 8));
    }
    uint16_t get(int i) const { return to565(raw(i)); }

private:
    uint8_t _b[N / 2 * 3];
};
