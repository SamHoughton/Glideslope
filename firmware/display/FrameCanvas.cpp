#include "display/FrameCanvas.h"
#include "display/Font5x7.h"

void FrameCanvas::fillRect(int x, int y, int w, int h, Rgb c)
{
    const uint16_t v = pack(c);
    for (int yy = y; yy < y + h; ++yy)
        for (int xx = x; xx < x + w; ++xx)
            set(xx, yy, v);
}

void FrameCanvas::text(int x, int y, const char *s, Rgb c, int scale, int pitch, int maxChars)
{
    if (!s) return;
    if (pitch <= 0) pitch = 6 * scale;
    const uint16_t v = pack(c);
    char ch;
    for (int i = 0, n; (n = Font5x7::next(s, ch)) && (maxChars < 0 || i < maxChars); ++i, s += n, x += pitch)
    {
        const uint8_t *rows = Font5x7::find(ch);
        if (!rows) continue;
        for (int r = 0; r < Font5x7::GLYPH_H; ++r)
            for (int col = 0; col < Font5x7::GLYPH_W; ++col)
                if (rows[r] & (0x10 >> col))
                    for (int dy = 0; dy < scale; ++dy)
                        for (int dx = 0; dx < scale; ++dx)
                            set(x + col * scale + dx, y + r * scale + dy, v);
    }
}

int FrameCanvas::textWidth(const char *s, int scale, int pitch, int maxChars)
{
    if (!s) return 0;
    int n = 0;   // characters, not bytes (accented letters are two bytes)
    char ch;
    for (int k; (k = Font5x7::next(s, ch)); s += k) ++n;
    if (maxChars >= 0 && n > maxChars) n = maxChars;
    if (n == 0) return 0;
    if (pitch <= 0) pitch = 6 * scale;
    return n * pitch - (pitch - Font5x7::GLYPH_W * scale);
}
