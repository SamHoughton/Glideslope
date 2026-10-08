#!/usr/bin/env python3
"""
Glideslope brand marks, drawn the way the panel draws: LED dots on a grid.
Writes SVG (and PNG previews) of the icon, the wordmark, the lock-up and the
social-preview card into brand/.

Usage:  python tools/brand_workbench.py [concept]      concept: a, b or c (default: all, side by side)

Icon grids are text: '.' off, and a letter per colour (see COLOURS).
The wordmark uses the panel's own 5x7 font (firmware/display/Font5x7.h).
"""
import re
import sys
from pathlib import Path
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / 'brand'

BG      = (13, 17, 23)       # GitHub dark, close to an unlit panel
UNLIT   = (24, 29, 38)
COLOURS = {
    'a': (255, 185, 60),     # amber: the glideslope (the panel's value colour)
    'w': (235, 240, 245),    # white: aircraft, runway keys
    'b': (90, 170, 255),     # blue: tail fin (the default accent)
    'g': (95, 100, 112),     # grey: runway surface
    'r': (255, 70, 60),      # red: PAPI
    'd': (120, 90, 30),      # dim amber: beam edges
    'c': (60, 200, 210),     # cyan: approach lights
}

# Concept A, "Approach": the plane riding a dotted 3-degree path down to a
# runway, PAPI lights (two white, two red: on the slope) beside it.
ICON_A = [
    "....................",
    "....................",
    ".a..................",
    "...a................",
    ".....a..............",
    ".......b............",
    ".......bb...........",
    ".......wwwwwww......",
    "........wwwwwwww....",
    "..........ww....a...",
    "..................a.",
    "....................",
    "....................",
    "..wwrr..............",
    "....................",
    ".gggggggggggggggggg.",
    ".gwgwgwg..g..g..g.g.",
    ".gggggggggggggggggg.",
    "....................",
    "....................",
]

# Concept B, "LED G": a dotted G whose bar runs out as the glideslope onto a
# runway.
ICON_B = [
    "........................",
    "........................",
    "......aaaaaaaa..........",
    ".....aaaaaaaaaa.........",
    "....aa........aa........",
    "...aa...................",
    "...aa...................",
    "...aa...................",
    "...aa.......aaaaaa......",
    "...aa.......aaaaaa......",
    "...aa...........aa......",
    "....aa..........aa......",
    ".....aaaaaaaaaaaaa......",
    "......aaaaaaaaaa.a......",
    "...................a....",
    "....................a...",
    ".....................a..",
    "........................",
    "..............gggggggggg",
    "..............gwgwg..g..",
    "..............gggggggggg",
    "........................",
    "........................",
    "........................",
]

# Concept C, "Beam": the ILS beam narrowing to the threshold, the aircraft
# centred in it.
ICON_C = [
    "........................",
    "d.......................",
    "..d.....................",
    "....d...................",
    "d.....d.................",
    "..d......d..............",
    "....d.......d...........",
    "......d........d........",
    "........d.....b...d.....",
    "..........d..wbb.....d..",
    "......wwwwwwwwww........",
    ".......wwwwwwwww..d.....",
    ".........ww.d......d....",
    "..............d.....d...",
    "................d....d..",
    "..................d...d.",
    "....................d.d.",
    "......................a.",
    "........................",
    "....cccc.gggggggggggggg.",
    ".........gwgwg..g..g..g.",
    ".........gggggggggggggg.",
    "........................",
    "........................",
]

CONCEPTS = {'a': ICON_A, 'b': ICON_B, 'c': ICON_C}


def font():
    src = (ROOT / 'firmware' / 'display' / 'Font5x7.h').read_text(encoding='utf-8')
    glyphs = {}
    for ch, rows in re.findall(r"\{'(.)',\s*\{([0-9 ,]+)\}\}", src):
        glyphs[ch] = [int(v) for v in rows.split(',')]
    return glyphs


def text_grid(text, colour='w'):
    g = font()
    rows = [''] * 7
    for i, ch in enumerate(text.upper()):
        bits = g.get(ch, [0] * 7)
        for r in range(7):
            rows[r] += ''.join(colour if bits[r] & (0x10 >> c) else '.' for c in range(5))
            if i < len(text) - 1:
                rows[r] += '.'
    return rows


def dots_svg(grid, cell, x0=0, y0=0, unlit=True):
    out = []
    rad = cell * 0.42
    for y, row in enumerate(grid):
        for x, ch in enumerate(row):
            cx, cy = x0 + (x + 0.5) * cell, y0 + (y + 0.5) * cell
            if ch == '.':
                if unlit:
                    out.append(f'<circle cx="{cx:.1f}" cy="{cy:.1f}" r="{cell*0.16:.1f}" fill="rgb{UNLIT}"/>')
                continue
            out.append(f'<circle cx="{cx:.1f}" cy="{cy:.1f}" r="{rad:.1f}" fill="rgb{COLOURS[ch]}"/>')
    return '\n'.join(out)


def icon_svg(grid, size=512):
    n = len(grid)
    cell = size / (n + 2)
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {size} {size}" width="{size}" height="{size}">\n'
            f'<rect width="{size}" height="{size}" rx="{size*0.18:.0f}" fill="rgb{BG}"/>\n'
            f'{dots_svg(grid, cell, cell, cell)}\n</svg>\n')


def wordmark():
    a, b = text_grid('Glide', 'w'), text_grid('slope', 'a')
    return [ra + '.' + rb for ra, rb in zip(a, b)]


def lockup_svg(icon, word_colour='w', height=160):
    word = wordmark()
    n = len(icon)
    cell = height / (n + 2)
    wcell = cell * 1.7
    word_w = len(word[0]) * wcell
    w = height + height * 0.25 + word_w + height * 0.25
    wy = (height - 7 * wcell) / 2
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w:.0f} {height}" width="{w:.0f}" height="{height}">\n'
            f'<rect width="{w:.0f}" height="{height}" rx="{height*0.18:.0f}" fill="rgb{BG}"/>\n'
            f'{dots_svg(icon, cell, cell, cell, unlit=False)}\n'
            f'{dots_svg(word, wcell, height + height * 0.1, wy, unlit=False)}\n</svg>\n')


# PNG rendering with the same geometry (no SVG renderer needed).
def dots_png(d, grid, cell, x0, y0, unlit=True):
    rad = cell * 0.42
    for y, row in enumerate(grid):
        for x, ch in enumerate(row):
            cx, cy = x0 + (x + 0.5) * cell, y0 + (y + 0.5) * cell
            if ch == '.':
                if unlit:
                    r = cell * 0.16
                    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=UNLIT)
                continue
            d.ellipse([cx - rad, cy - rad, cx + rad, cy + rad], fill=COLOURS[ch])


def icon_png(grid, size=512):
    ss = 4
    im = Image.new('RGBA', (size * ss, size * ss), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([0, 0, size * ss - 1, size * ss - 1], radius=int(size * ss * 0.18), fill=BG)
    n = len(grid)
    cell = size * ss / (n + 2)
    dots_png(d, grid, cell, cell, cell)
    return im.resize((size, size), Image.LANCZOS)


def lockup_png(icon, height=160, word_colour='w', bg=True):
    ss = 4
    word = wordmark()
    n = len(icon)
    cell = height * ss / (n + 2)
    wcell = cell * 1.7
    w = int(height * ss * 1.5 + len(word[0]) * wcell)
    im = Image.new('RGBA', (w, height * ss), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    if bg:
        d.rounded_rectangle([0, 0, w - 1, height * ss - 1], radius=int(height * ss * 0.18), fill=BG)
    dots_png(d, icon, cell, cell, cell, unlit=False)
    dots_png(d, word, wcell, height * ss * 1.1, (height * ss - 7 * wcell) / 2, unlit=False)
    return im.resize((w // ss, height), Image.LANCZOS)


def demo_logo_header(grid):
    """32x32 RGB565 badge for the showcase flight's card (the icon, 1 px a dot)."""
    def pack(c):
        return ((c[0] & 0xF8) << 8) | ((c[1] & 0xFC) << 3) | (c[2] >> 3)
    n = len(grid)
    off = (32 - n) // 2
    px = []
    for y in range(32):
        for x in range(32):
            gx, gy = x - off, y - off
            ch = grid[gy][gx] if 0 <= gx < n and 0 <= gy < n else '.'
            px.append(pack(COLOURS[ch]) if ch != '.' else pack(BG))
    rows = ',\n    '.join(', '.join(f'0x{v:04X}' for v in px[i:i + 16]) for i in range(0, len(px), 16))
    return ('// Generated by tools/brand_workbench.py: do not edit by hand.\n'
            '// The Glideslope icon as a 32x32 RGB565 airline badge for the showcase flight.\n'
            '#pragma once\n#include <stdint.h>\n\n'
            f'static const uint16_t kDemoLogo[32 * 32] = {{\n    {rows}\n}};\n')


def mark_svg(grid, size=24):
    """Compact icon (lit dots only) for the web page header and favicon."""
    n = len(grid)
    cell = size / (n + 2)
    return (f"<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 {size} {size}' width='{size}' height='{size}'>"
            f"<rect width='{size}' height='{size}' rx='{size * 0.18:.1f}' fill='rgb{BG}'/>"
            + ''.join(f"<circle cx='{cell * (x + 1.5):.2f}' cy='{cell * (y + 1.5):.2f}' r='{cell * 0.42:.2f}' fill='rgb{COLOURS[ch]}'/>"
                      for y, row in enumerate(grid) for x, ch in enumerate(row) if ch != '.')
            + '</svg>')


def brand_mark_header(grid):
    svg = mark_svg(grid)
    fav = svg.replace("'", '%27').replace('<', '%3C').replace('>', '%3E')
    return ('// Generated by tools/brand_workbench.py: do not edit by hand.\n'
            '// The Glideslope icon as inline SVG (web page header) and as a\n'
            '// data-URI favicon. Macros, so they concatenate with the page literal.\n'
            '#pragma once\n\n'
            f'#define GS_BRAND_SVG "{svg}"\n'
            f'#define GS_FAVICON_URI "data:image/svg+xml,{fav}"\n')


def social_card():
    """1280x640 GitHub social preview: lock-up, tagline, and the panel showing
    the showcase card (docs/showcase-card.png from tools/record_demo.py)."""
    from PIL import ImageFont
    W_, H_ = 1280, 640
    card = Image.new('RGB', (W_, H_), BG)
    d = ImageDraw.Draw(card)
    # Faint LED grid across the background, like an unlit panel.
    for y in range(8, H_, 16):
        for x in range(8, W_, 16):
            d.ellipse([x - 1.5, y - 1.5, x + 1.5, y + 1.5], fill=(20, 25, 33))
    lock = lockup_png(ICON_A, 96, bg=False)
    card.paste(lock, (64, 64), lock)

    def font(names, size):
        for n in names:
            try:
                return ImageFont.truetype(n, size)
            except OSError:
                continue
        return ImageFont.load_default()
    bold = font(['segoeuib.ttf', 'DejaVuSans-Bold.ttf'], 44)
    body = font(['segoeui.ttf', 'DejaVuSans.ttf'], 28)
    d.text((70, 200), 'An LED flight board for the planes', font=bold, fill=(235, 240, 245))
    d.text((70, 252), 'landing over your house.', font=bold, fill=(255, 185, 60))
    for i, line in enumerate(['Live ADS-B: landings, go-arounds,',
                              'take-offs, a map, arrivals and weather',
                              'on a 128×64 LED panel.']):
        d.text((72, 330 + i * 40), line, font=body, fill=(139, 148, 158))
    shot = ROOT / 'docs' / 'showcase-card.png'
    if shot.exists():
        panel = Image.open(shot).convert('RGB')
        panel = panel.resize((520, int(panel.height * 520 / panel.width)), Image.LANCZOS)
        card.paste(panel, (W_ - panel.width - 56, H_ - panel.height - 56))
    d.text((72, H_ - 76), 'github.com/SamHoughton/Glideslope', font=body, fill=(88, 166, 255))
    card.save(OUT / 'social-preview.png', optimize=True)
    print('wrote brand/social-preview.png')


def main():
    OUT.mkdir(exist_ok=True)
    pick = sys.argv[1].lower() if len(sys.argv) > 1 else None
    if pick == 'social':
        social_card()
        return
    if pick:
        grid = CONCEPTS[pick]
        (OUT / 'glideslope-icon.svg').write_text(icon_svg(grid), encoding='utf-8')
        (OUT / 'glideslope-lockup.svg').write_text(lockup_svg(grid), encoding='utf-8')
        icon_png(grid, 512).save(OUT / 'glideslope-icon-512.png')
        icon_png(grid, 64).save(OUT / 'favicon-64.png')
        lockup_png(grid, 160).save(OUT / 'glideslope-lockup.png')
        (ROOT / 'firmware' / 'display' / 'DemoLogo.h').write_text(demo_logo_header(grid), encoding='utf-8')
        (ROOT / 'firmware' / 'utils' / 'BrandMark.h').write_text(brand_mark_header(grid), encoding='utf-8')
        (OUT / 'favicon.svg').write_text(mark_svg(grid, 64), encoding='utf-8')
        print('wrote brand/ for concept', pick)
        return
    # Comparison sheet of all concepts.
    sheet = Image.new('RGB', (3 * 300 + 40, 300 + 200), (30, 34, 42))
    for i, (k, grid) in enumerate(CONCEPTS.items()):
        sheet.paste(icon_png(grid, 260), (20 + i * 300, 20), icon_png(grid, 260))
        small = icon_png(grid, 48)
        sheet.paste(small, (20 + i * 300, 300), small)
        lock = lockup_png(grid, 60)
        lock = lock.resize((min(280, lock.width), int(lock.height * min(280, lock.width) / lock.width)))
        sheet.paste(lock, (20 + i * 300, 370), lock)
        ImageDraw.Draw(sheet).text((80 + i * 300, 310), f'concept {k.upper()}', fill=(200, 200, 200))
    sheet.save(OUT / 'concepts.png')
    print('wrote brand/concepts.png')


if __name__ == '__main__':
    main()
