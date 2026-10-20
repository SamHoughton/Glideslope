#!/usr/bin/env python3
"""Generate firmware/display/Fonts.cpp: the 7.0 typefaces.

  Label   a hand-drawn proportional pixel font (7 px capitals, lowercase with
          descenders), crisp at 1:1 on the LEDs; drawn below as ASCII art.
  Medium, Large, Huge
          Barlow Semi Condensed Bold (SIL Open Font Licence, Jeremy Tribby),
          rasterised with soft edges at three sizes for big numbers and titles.

Every glyph is stored as 4-bit coverage (16 levels) so one renderer draws all
of them, blending into whatever is underneath.

The Barlow files are not kept in this repository. Fetch them first:
  https://github.com/google/fonts/raw/main/ofl/barlowsemicondensed/BarlowSemiCondensed-Bold.ttf

  python tools/gen_fonts.py DIR_WITH_TTF
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

OUT = os.path.join(os.path.dirname(__file__), '..', 'firmware', 'display', 'Fonts.cpp')

# ── Label: rows 0-6 are the capitals (row 6 the baseline), 7-8 descenders ──
# Lowercase x-height is rows 2-6. '#' lit, anything else dark.
LABEL = {
' ': ['..'] * 7,
'A': ['.##.', '#..#', '#..#', '####', '#..#', '#..#', '#..#'],
'B': ['###.', '#..#', '#..#', '###.', '#..#', '#..#', '###.'],
'C': ['.##.', '#..#', '#...', '#...', '#...', '#..#', '.##.'],
'D': ['###.', '#..#', '#..#', '#..#', '#..#', '#..#', '###.'],
'E': ['####', '#...', '#...', '###.', '#...', '#...', '####'],
'F': ['####', '#...', '#...', '###.', '#...', '#...', '#...'],
'G': ['.##.', '#..#', '#...', '#.##', '#..#', '#..#', '.###'],
'H': ['#..#', '#..#', '#..#', '####', '#..#', '#..#', '#..#'],
'I': ['###', '.#.', '.#.', '.#.', '.#.', '.#.', '###'],
'J': ['...#', '...#', '...#', '...#', '#..#', '#..#', '.##.'],
'K': ['#..#', '#..#', '#.#.', '##..', '#.#.', '#..#', '#..#'],
'L': ['#...', '#...', '#...', '#...', '#...', '#...', '####'],
'M': ['#...#', '##.##', '#.#.#', '#.#.#', '#...#', '#...#', '#...#'],
'N': ['#..#', '##.#', '##.#', '#.##', '#.##', '#..#', '#..#'],
'O': ['.###.', '#...#', '#...#', '#...#', '#...#', '#...#', '.###.'],
'P': ['###.', '#..#', '#..#', '###.', '#...', '#...', '#...'],
'Q': ['.##.', '#..#', '#..#', '#..#', '#..#', '#.#.', '.#.#'],
'R': ['###.', '#..#', '#..#', '###.', '#.#.', '#..#', '#..#'],
'S': ['.##.', '#..#', '#...', '.##.', '...#', '#..#', '.##.'],
'T': ['#####', '..#..', '..#..', '..#..', '..#..', '..#..', '..#..'],
'U': ['#..#', '#..#', '#..#', '#..#', '#..#', '#..#', '.##.'],
'V': ['#...#', '#...#', '#...#', '#...#', '.#.#.', '.#.#.', '..#..'],
'W': ['#...#', '#...#', '#...#', '#.#.#', '#.#.#', '##.##', '#...#'],
'X': ['#...#', '#...#', '.#.#.', '..#..', '.#.#.', '#...#', '#...#'],
'Y': ['#...#', '#...#', '.#.#.', '..#..', '..#..', '..#..', '..#..'],
'Z': ['####', '...#', '...#', '..#.', '.#..', '#...', '####'],
'0': ['.##.', '#..#', '#..#', '#..#', '#..#', '#..#', '.##.'],
'1': ['.#.', '##.', '.#.', '.#.', '.#.', '.#.', '###'],
'2': ['.##.', '#..#', '...#', '..#.', '.#..', '#...', '####'],
'3': ['.##.', '#..#', '...#', '.##.', '...#', '#..#', '.##.'],
'4': ['..#.', '.##.', '#.#.', '#.#.', '####', '..#.', '..#.'],
'5': ['####', '#...', '###.', '...#', '...#', '#..#', '.##.'],
'6': ['.##.', '#...', '#...', '###.', '#..#', '#..#', '.##.'],
'7': ['####', '...#', '...#', '..#.', '..#.', '.#..', '.#..'],
'8': ['.##.', '#..#', '#..#', '.##.', '#..#', '#..#', '.##.'],
'9': ['.##.', '#..#', '#..#', '.###', '...#', '...#', '.##.'],
'a': ['....', '....', '.##.', '...#', '.###', '#..#', '.###'],
'b': ['#...', '#...', '###.', '#..#', '#..#', '#..#', '###.'],
'c': ['...', '...', '.##', '#..', '#..', '#..', '.##'],
'd': ['...#', '...#', '.###', '#..#', '#..#', '#..#', '.###'],
'e': ['....', '....', '.##.', '#..#', '####', '#...', '.##.'],
'f': ['.##', '#..', '###', '#..', '#..', '#..', '#..'],
'g': ['....', '....', '.###', '#..#', '#..#', '#..#', '.###', '...#', '.##.'],
'h': ['#...', '#...', '###.', '#..#', '#..#', '#..#', '#..#'],
'i': ['#', '.', '#', '#', '#', '#', '#'],
'j': ['.#', '..', '.#', '.#', '.#', '.#', '.#', '.#', '#.'],
'k': ['#...', '#...', '#..#', '#.#.', '##..', '#.#.', '#..#'],
'l': ['#.', '#.', '#.', '#.', '#.', '#.', '.#'],
'm': ['.....', '.....', '####.', '#.#.#', '#.#.#', '#.#.#', '#.#.#'],
'n': ['....', '....', '###.', '#..#', '#..#', '#..#', '#..#'],
'o': ['....', '....', '.##.', '#..#', '#..#', '#..#', '.##.'],
'p': ['....', '....', '###.', '#..#', '#..#', '#..#', '###.', '#...', '#...'],
'q': ['....', '....', '.###', '#..#', '#..#', '#..#', '.###', '...#', '...#'],
'r': ['...', '...', '#.#', '##.', '#..', '#..', '#..'],
's': ['....', '....', '.###', '#...', '.##.', '...#', '###.'],
't': ['...', '.#.', '###', '.#.', '.#.', '.#.', '..#'],
'u': ['....', '....', '#..#', '#..#', '#..#', '#..#', '.###'],
'v': ['.....', '.....', '#...#', '#...#', '.#.#.', '.#.#.', '..#..'],
'w': ['.....', '.....', '#...#', '#...#', '#.#.#', '#.#.#', '.#.#.'],
'x': ['....', '....', '#..#', '#..#', '.##.', '#..#', '#..#'],
'y': ['....', '....', '#..#', '#..#', '#..#', '#..#', '.###', '...#', '.##.'],
'z': ['....', '....', '####', '..#.', '.#..', '#...', '####'],
':': ['.', '.', '#', '.', '.', '#', '.'],
'.': ['.', '.', '.', '.', '.', '.', '#'],
',': ['..', '..', '..', '..', '..', '.#', '.#', '#.'],
'-': ['...', '...', '...', '###', '...', '...', '...'],
'+': ['...', '...', '.#.', '###', '.#.', '...', '...'],
'/': ['..#', '..#', '.#.', '.#.', '.#.', '#..', '#..'],
'(': ['.#', '#.', '#.', '#.', '#.', '#.', '.#'],
')': ['#.', '.#', '.#', '.#', '.#', '.#', '#.'],
"'": ['#', '#', '.', '.', '.', '.', '.'],
'%': ['##..#', '##.#.', '...#.', '..#..', '.#...', '.#.##', '#..##'],
'>': ['#..', '.#.', '..#', '..#', '..#', '.#.', '#..'],
'<': ['..#', '.#.', '#..', '#..', '#..', '.#.', '..#'],
'!': ['#', '#', '#', '#', '#', '.', '#'],
'?': ['.##.', '#..#', '...#', '..#.', '.#..', '....', '.#..'],
'&': ['.#..', '#.#.', '#.#.', '.#..', '#.##', '#..#', '.##.'],
'*': ['.....', '#.#.#', '.###.', '#####', '.###.', '#.#.#', '.....'],
'#': ['.#.#.', '.#.#.', '#####', '.#.#.', '#####', '.#.#.', '.#.#.'],
'=': ['...', '...', '###', '...', '###', '...', '...'],
'~': ['.....', '..#..', '...#.', '#####', '...#.', '..#..', '.....'],   # an arrow (also UTF-8 "→")
'\x7f': ['.#.', '#.#', '.#.', '...', '...', '...', '...'],   # degree sign
}

FIRST, LAST = 32, 127   # printable ASCII and the degree sign (stored at 127)


def label_font():
    glyphs = {}
    for ch, rows in LABEL.items():
        w = max(len(r) for r in rows)
        px = [[15 if c == '#' else 0 for c in r.ljust(w, '.')] for r in rows]
        glyphs[ord(ch)] = dict(w=w, h=len(rows), dx=0, dy=0, adv=w + 1, px=px)
    return dict(name='Label', cap=7, line=10, glyphs=glyphs)


def ttf_font(name, path, size, chars):
    f = ImageFont.truetype(path, size)
    # Capital height from 'H', measured from the drawing origin.
    hb = f.getbbox('H')
    cap_top, base = hb[1], hb[3]
    glyphs = {}
    for ch in chars:
        code = 127 if ch == '°' else ord(ch)
        adv = int(round(f.getlength(ch)))
        bb = f.getbbox(ch)
        if bb[2] <= bb[0] or bb[3] <= bb[1]:
            glyphs[code] = dict(w=0, h=0, dx=0, dy=0, adv=adv, px=[])
            continue
        im = Image.new('L', (bb[2] - bb[0], bb[3] - bb[1]))
        ImageDraw.Draw(im).text((-bb[0], -bb[1]), ch, font=f, fill=255)
        w, h = im.size
        px = []
        for y in range(h):
            row = []
            for x in range(w):
                a = im.getpixel((x, y))
                # A little extra contrast: LEDs make faint edge pixels look
                # like noise, and nearly-full ones should simply be full.
                a = max(0, min(255, (a - 30) * 255 // 190))
                row.append((a * 15 + 127) // 255)
            px.append(row)
        glyphs[code] = dict(w=w, h=h, dx=bb[0], dy=bb[1] - cap_top, adv=adv, px=px)
    return dict(name=name, cap=base - cap_top, line=int(round((base - cap_top) * 1.45)), glyphs=glyphs)


def emit(fonts):
    out = ['// Generated by tools/gen_fonts.py; do not edit.',
           '// Medium, Large and Huge: Barlow Semi Condensed Bold, (c) 2017 The Barlow',
           '// Project Authors, SIL Open Font License 1.1 (display/Fonts-OFL.txt).',
           '#include "display/Fonts.h"', '', 'namespace Fonts', '{']
    for font in fonts:
        n = font['name']
        data, table = [], []
        for code in range(FIRST, LAST + 1):
            g = font['glyphs'].get(code)
            if g is None:
                table.append('    {0, 0, 0, 0, 0, 0}')
                continue
            off = len(data)
            flat = [v for row in g['px'] for v in row]
            if len(flat) % 2: flat.append(0)
            data += [(flat[i] << 4) | flat[i + 1] for i in range(0, len(flat), 2)]
            table.append('    {%d, %d, %d, %d, %d, %d}' % (off, g['w'], g['h'], g['dx'], g['dy'], g['adv']))
        assert len(data) < 65536
        out.append('static const uint8_t k%sPx[] = {' % n)
        for i in range(0, len(data), 24):
            out.append('    ' + ', '.join('0x%02x' % b for b in data[i:i + 24]) + ',')
        out.append('};')
        out.append('static const Glyph k%sGlyphs[] = {' % n)
        out.append(',\n'.join(table))
        out.append('};')
        out.append('const Font %s = {k%sGlyphs, k%sPx, %d, %d, %d, %d};' % (n, n, n, FIRST, LAST, font['cap'], font['line']))
        out.append('')
    out.append('}')
    open(OUT, 'w', encoding='utf-8', newline='\n').write('\n'.join(out) + '\n')


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else '.'
    bold = os.path.join(d, 'BarlowSemiCondensed-Bold.ttf')
    big = ''.join(chr(c) for c in range(32, 127)) + '°'
    fonts = [label_font(),
             ttf_font('Medium', bold, 13, big),
             ttf_font('Large', bold, 18, big),
             ttf_font('Huge', bold, 25, '0123456789:.-+/° ')]
    for f in fonts:
        print(f['name'], 'cap', f['cap'], 'line', f['line'])
    emit(fonts)


if __name__ == '__main__':
    main()
