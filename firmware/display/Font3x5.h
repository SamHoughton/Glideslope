#pragma once
/*
Purpose: A 3x5 pixel font (digits, capitals, a few marks) for labels that
must be small: the flight numbers on the map. Four pixels a character.
B (square corners on the left only) and 8, and 0 and O (rounded), are
kept apart so a flight number like BA80 can't be misread.
*/
#include "display/FrameCanvas.h"

namespace Font3x5
{
struct Glyph { char ch; uint8_t rows[5]; };   // 3 bits a row, MSB on the left

static const Glyph kGlyphs[] = {
    {'0', {7, 5, 5, 5, 7}}, {'1', {2, 6, 2, 2, 7}}, {'2', {7, 1, 7, 4, 7}}, {'3', {7, 1, 3, 1, 7}},
    {'4', {5, 5, 7, 1, 1}}, {'5', {7, 4, 7, 1, 7}}, {'6', {7, 4, 7, 5, 7}}, {'7', {7, 1, 1, 2, 2}},
    {'8', {7, 5, 7, 5, 7}}, {'9', {7, 5, 7, 1, 7}},
    {'A', {2, 5, 7, 5, 5}}, {'B', {6, 5, 6, 5, 6}}, {'C', {3, 4, 4, 4, 3}}, {'D', {6, 5, 5, 5, 6}},
    {'E', {7, 4, 6, 4, 7}}, {'F', {7, 4, 6, 4, 4}}, {'G', {3, 4, 5, 5, 3}}, {'H', {5, 5, 7, 5, 5}},
    {'I', {7, 2, 2, 2, 7}}, {'J', {1, 1, 1, 5, 2}}, {'K', {5, 5, 6, 5, 5}}, {'L', {4, 4, 4, 4, 7}},
    {'M', {5, 7, 7, 5, 5}}, {'N', {6, 5, 5, 5, 5}}, {'O', {2, 5, 5, 5, 2}}, {'P', {6, 5, 6, 4, 4}},
    {'Q', {2, 5, 5, 7, 3}}, {'R', {6, 5, 6, 5, 5}}, {'S', {3, 4, 2, 1, 6}}, {'T', {7, 2, 2, 2, 2}},
    {'U', {5, 5, 5, 5, 7}}, {'V', {5, 5, 5, 5, 2}}, {'W', {5, 5, 7, 7, 5}}, {'X', {5, 5, 2, 5, 5}},
    {'Y', {5, 5, 2, 2, 2}}, {'Z', {7, 1, 2, 4, 7}}, {'-', {0, 0, 7, 0, 0}}, {'.', {0, 0, 0, 0, 2}},
};

inline const uint8_t *find(char ch)
{
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
    for (const Glyph &g : kGlyphs)
        if (g.ch == ch) return g.rows;
    return nullptr;   // space or unknown: blank
}

inline int width(const char *s)
{
    const int n = (int)strlen(s);
    return n ? n * 4 - 1 : 0;
}

// shadow: a dark copy one pixel down and right, so it reads over the map.
inline void text(FrameCanvas &c, int x, int y, const char *s, Rgb col, bool shadow = true)
{
    for (int pass = shadow ? 0 : 1; pass < 2; ++pass)
        for (int i = 0; s[i]; ++i)
        {
            const uint8_t *g = find(s[i]);
            if (!g) continue;
            for (int r = 0; r < 5; ++r)
                for (int b = 0; b < 3; ++b)
                    if (g[r] & (4 >> b))
                    {
                        const int px = x + i * 4 + b + (pass ? 0 : 1), py = y + r + (pass ? 0 : 1);
                        if (px >= 0 && py >= 0 && px < FrameCanvas::W && py < FrameCanvas::H)
                            c.set(px, py, pass ? col : Rgb{0, 0, 0});
                    }
        }
}
}
