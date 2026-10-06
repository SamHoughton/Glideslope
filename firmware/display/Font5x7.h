#pragma once
/*
Purpose: 5x7 bitmap font for the flight card.
Each glyph is 7 rows, top to bottom; bit 4 is the leftmost of 5 columns.
Lower-case letters render as upper case. Unknown characters render blank.
*/
#include <stdint.h>

namespace Font5x7
{
    static constexpr int GLYPH_W = 5;
    static constexpr int GLYPH_H = 7;

    struct Glyph { char ch; uint8_t rows[7]; };

    static const Glyph kGlyphs[] = {
        {'A', {14,17,17,31,17,17,17}}, {'B', {30,17,17,30,17,17,30}}, {'C', {14,17,16,16,16,17,14}},
        {'D', {28,18,17,17,17,18,28}}, {'E', {31,16,16,30,16,16,31}}, {'F', {31,16,16,30,16,16,16}},
        {'G', {14,17,16,23,17,17,15}}, {'H', {17,17,17,31,17,17,17}}, {'I', {14, 4, 4, 4, 4, 4,14}},
        {'J', { 7, 2, 2, 2, 2,18,12}}, {'K', {17,18,20,24,20,18,17}}, {'L', {16,16,16,16,16,16,31}},
        {'M', {17,27,21,21,17,17,17}}, {'N', {17,17,25,21,19,17,17}}, {'O', {14,17,17,17,17,17,14}},
        {'P', {30,17,17,30,16,16,16}}, {'Q', {14,17,17,17,21,18,13}}, {'R', {30,17,17,30,20,18,17}},
        {'S', {15,16,16,14, 1, 1,30}}, {'T', {31, 4, 4, 4, 4, 4, 4}}, {'U', {17,17,17,17,17,17,14}},
        {'V', {17,17,17,17,17,10, 4}}, {'W', {17,17,17,21,21,21,10}}, {'X', {17,17,10, 4,10,17,17}},
        {'Y', {17,17,10, 4, 4, 4, 4}}, {'Z', {31, 1, 2, 4, 8,16,31}},
        {'0', {14,17,19,21,25,17,14}}, {'1', { 4,12, 4, 4, 4, 4,14}}, {'2', {14,17, 1, 2, 4, 8,31}},
        {'3', {31, 2, 4, 2, 1,17,14}}, {'4', { 2, 6,10,18,31, 2, 2}}, {'5', {31,16,30, 1, 1,17,14}},
        {'6', { 6, 8,16,30,17,17,14}}, {'7', {31, 1, 2, 4, 8, 8, 8}}, {'8', {14,17,17,14,17,17,14}},
        {'9', {14,17,17,15, 1, 2,12}},
        {'>', { 8, 4, 2, 1, 2, 4, 8}}, {'<', { 2, 4, 8,16, 8, 4, 2}}, {':', { 0,12,12, 0,12,12, 0}},
        {'-', { 0, 0, 0,31, 0, 0, 0}}, {'+', { 0, 4, 4,31, 4, 4, 0}}, {'.', { 0, 0, 0, 0, 0,12,12}},
        {',', { 0, 0, 0, 0,12, 4, 8}}, {'/', { 1, 1, 2, 4, 8,16,16}}, {'(', { 2, 4, 8, 8, 8, 4, 2}},
        {')', { 8, 4, 2, 2, 2, 4, 8}}, {'\'',{ 4, 4, 8, 0, 0, 0, 0}}, {'&', {12,18,20, 8,21,18,13}},
        {'!', { 4, 4, 4, 4, 4, 0, 4}}, {'?', {14,17, 1, 2, 4, 0, 4}}, {'#', {10,10,31,10,31,10,10}},
        {'%', {24,25, 2, 4, 8,19, 3}}, {'*', {12,18,18,12, 0, 0, 0}},  // '*' doubles as a degree sign
    };

    // Reads one character at s, folding UTF-8 Latin-1 accented letters to
    // their plain capital (é -> E, ñ -> N). Returns bytes consumed (0 at end).
    inline int next(const char *s, char &out)
    {
        const uint8_t b = (uint8_t)s[0];
        if (!b) return 0;
        if (b == 0xC3 && s[1])
        {
            const uint8_t c = (uint8_t)s[1] & 0xDF;   // fold lower case (0xA0-0xBF) onto upper (0x80-0x9F)
            out = c <= 0x85 ? 'A' : c == 0x87 ? 'C' : c <= 0x8B ? 'E' : c <= 0x8F ? 'I'
                : c == 0x91 ? 'N' : (c <= 0x96 || c == 0x98) ? 'O' : (c >= 0x99 && c <= 0x9C) ? 'U'
                : c == 0x9D ? 'Y' : '?';
            return 2;
        }
        out = (char)b;
        if (b >= 0x80)   // other multi-byte sequences: skip the continuation bytes
        {
            int n = 1;
            while ((((uint8_t)s[n]) & 0xC0) == 0x80) ++n;
            out = '?';
            return n;
        }
        return 1;
    }

    // Returns the 7 row bitmaps for ch, or nullptr for a blank (space or unknown).
    inline const uint8_t *find(char ch)
    {
        if (ch >= 'a' && ch <= 'z') ch = ch - 'a' + 'A';
        for (const Glyph &g : kGlyphs)
            if (g.ch == ch) return g.rows;
        return nullptr;
    }
}
