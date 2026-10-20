#include "display/FrameCanvas.h"
#include "display/Font5x7.h"
#include "display/Fonts.h"

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

void FrameCanvas::blend(int x, int y, Rgb c, uint8_t a)
{
    if (a == 0 || x < _clipX0 || x > _clipX1 || (unsigned)y >= (unsigned)H) return;
    if (a >= 15) { _px[y * W + x] = pack(c); return; }
    const Rgb d = unpack(_px[y * W + x]);
    _px[y * W + x] = pack({ (uint8_t)(d.r + ((int)c.r - d.r) * a / 15),
                            (uint8_t)(d.g + ((int)c.g - d.g) * a / 15),
                            (uint8_t)(d.b + ((int)c.b - d.b) * a / 15) });
}

int FrameCanvas::print(const Fonts::Font &f, int x, int y, const char *s, Rgb c, int tracking)
{
    if (!s) return 0;
    const int x0 = x;
    uint8_t ch;
    bool any = false;
    for (int n; (n = Fonts::next(s, ch)); s += n)
    {
        if (ch < f.first || ch > f.last) ch = '?';
        const Fonts::Glyph &g = f.glyphs[ch - f.first];
        if (!g.adv) continue;   // not in this font
        if (any) x += tracking;
        any = true;
        const uint8_t *p = f.px + g.off;
        for (int i = 0, n2 = g.w * g.h; i < n2; ++i)
        {
            const uint8_t a = (i & 1) ? (p[i >> 1] & 0x0F) : (p[i >> 1] >> 4);
            if (a) blend(x + g.dx + i % g.w, y + g.dy + i / g.w, c, a);
        }
        x += g.adv;
    }
    return x - x0;
}

int FrameCanvas::width(const Fonts::Font &f, const char *s, int tracking)
{
    if (!s) return 0;
    int w = 0, k = 0;
    uint8_t ch;
    for (int n; (n = Fonts::next(s, ch)); s += n)
    {
        if (ch < f.first || ch > f.last) ch = '?';
        const Fonts::Glyph &g = f.glyphs[ch - f.first];
        if (!g.adv) continue;
        w += g.adv + (k++ ? tracking : 0);
    }
    // The pixel font's advance includes a blank column after the last letter.
    return (&f == &Fonts::Label && w) ? w - 1 : w;
}
