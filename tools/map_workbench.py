#!/usr/bin/env python3
"""
Base layer for the London map mode: west and south-west London at 128x64,
equirectangular, ~350 m per pixel. Renders a preview PNG and writes
firmware/display/MapBase.h (palette-indexed, stored in flash).

Usage:  python tools/map_workbench.py [preview_dir]
"""
import sys
from pathlib import Path
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / 'firmware' / 'display' / 'MapBase.h'

W, H = 128, 64
LAT_N, LAT_S = 51.575, 51.375          # 0.2 deg = 22.2 km
LON_W, LON_E = -0.690, -0.0486         # 0.641 deg = 44.5 km at 51.5N -> square pixels

def xy(lat, lon):
    return ((lon - LON_W) / (LON_E - LON_W) * W, (LAT_N - lat) / (LAT_N - LAT_S) * H)

# Palette: index -> RGB (dim; aircraft are drawn bright on top)
PAL = [
    (0, 0, 0),        # 0 empty
    (20, 55, 110),    # 1 Thames
    (14, 34, 70),     # 2 reservoirs
    (95, 100, 112),   # 3 runways
    (24, 26, 32),     # 4 Heathrow apron
    (30, 30, 38),     # 5 M25 (dotted)
    (12, 40, 18),     # 6 parks
    (34, 34, 46),     # 7 approach lanes (dotted)
]
grid = [[0] * W for _ in range(H)]

def put(x, y, c):
    x, y = int(round(x)), int(round(y))
    if 0 <= x < W and 0 <= y < H:
        grid[y][x] = c

def line(p, q, c):
    (x0, y0), (x1, y1) = p, q
    n = int(max(abs(x1 - x0), abs(y1 - y0)) * 2) + 1
    for i in range(n + 1):
        t = i / n
        put(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, c)

def blob(lat, lon, rx, ry, c):
    cx, cy = xy(lat, lon)
    for y in range(int(cy - ry) - 1, int(cy + ry) + 2):
        for x in range(int(cx - rx) - 1, int(cx + rx) + 2):
            if ((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2 <= 1.0:
                put(x, y, c)

# Parks (approximate centres and extents, in pixels at ~350 m/px).
blob(51.4430, -0.2750, 3.7, 3.1, 6)   # Richmond Park
blob(51.4120, -0.3350, 2.6, 2.0, 6)   # Bushy Park
blob(51.4790, -0.2950, 1.4, 1.1, 6)   # Kew Gardens
blob(51.5070, -0.1650, 2.3, 1.1, 6)   # Hyde Park
blob(51.4130, -0.3850, 1.4, 1.1, 6)   # Kempton Park
blob(51.4420, -0.5700, 2.9, 2.6, 6)   # Windsor Great Park

# Reservoirs west of Heathrow (approximate centres and extents).
blob(51.4835, -0.5330, 1.4, 1.0, 2)   # Queen Mother
blob(51.4635, -0.5350, 1.0, 1.2, 2)   # Wraysbury
blob(51.4510, -0.4930, 1.6, 1.0, 2)   # King George VI
blob(51.4545, -0.4710, 1.0, 0.8, 2)   # Staines
blob(51.4300, -0.4250, 1.4, 1.2, 2)   # Queen Mary
blob(51.4100, -0.3900, 0.9, 0.7, 2)   # Knight / Bessborough area

# Thames, west to east (approximate waypoints).
THAMES = [
    (51.508, -0.701), (51.495, -0.650), (51.4855, -0.6085), (51.487, -0.590),
    (51.484, -0.576), (51.464, -0.577), (51.450, -0.560), (51.437, -0.546),
    (51.434, -0.512), (51.418, -0.503), (51.409, -0.488), (51.389, -0.488),
    (51.387, -0.459), (51.395, -0.416), (51.402, -0.406), (51.411, -0.369),
    (51.404, -0.343), (51.411, -0.309), (51.432, -0.323), (51.445, -0.323),
    (51.457, -0.306), (51.469, -0.322), (51.483, -0.306), (51.488, -0.288),
    (51.488, -0.279), (51.474, -0.270), (51.472, -0.253), (51.488, -0.230),
    (51.467, -0.213), (51.466, -0.188), (51.472, -0.180), (51.483, -0.166),
    (51.484, -0.150), (51.487, -0.127), (51.501, -0.122), (51.509, -0.117),
    (51.509, -0.104), (51.508, -0.088), (51.506, -0.075), (51.500, -0.050),
]
pts = [xy(a, b) for a, b in THAMES]
for p, q in zip(pts, pts[1:]):
    line(p, q, 1)
    # Wider through central London (east of Putney): a second, offset pass.
    if p[0] > xy(0, -0.215)[0]:
        line((p[0], p[1] + 1), (q[0], q[1] + 1), 1)

# M25, western side (approximate), drawn dotted so it stays in the background.
M25 = [(51.575, -0.493), (51.540, -0.498), (51.510, -0.503), (51.490, -0.510),
       (51.460, -0.520), (51.432, -0.524), (51.405, -0.531), (51.375, -0.505)]
m25 = [xy(a, b) for a, b in M25]
for p, q in zip(m25, m25[1:]):
    n = int(max(abs(q[0] - p[0]), abs(q[1] - p[1]))) + 1
    for i in range(0, n + 1, 2):
        t = i / n
        x, y = int(round(p[0] + (q[0] - p[0]) * t)), int(round(p[1] + (q[1] - p[1]) * t))
        if 0 <= x < W and 0 <= y < H and grid[y][x] == 0:
            grid[y][x] = 5

# Approach lanes: faint dotted extended centrelines, 25 km east of the 27L/27R
# thresholds (westerly arrivals) and west to the map edge for 09L/09R.
for lat, lon_thr, direction in ((51.4777, -0.4332, +1), (51.4649, -0.4340, +1),
                                (51.4775, -0.4850, -1), (51.4647, -0.4826, -1)):
    x0, y0 = xy(lat, lon_thr)
    length_px = (25.0 / 0.348) if direction > 0 else x0
    for i in range(2, int(length_px), 3):
        x = int(round(x0 + direction * i))
        if 0 <= x < W and 0 <= int(round(y0)) < H and grid[int(round(y0))][x] in (0, 5):
            grid[int(round(y0))][x] = 7

# Heathrow: a faint apron block, then the two runways on top, to scale.
for y in range(int(xy(51.4775, 0)[1]) + 1, int(xy(51.4650, 0)[1])):
    for x in range(int(xy(0, -0.480)[0]), int(xy(0, -0.437)[0]) + 1):
        put(x, y, 4)
line(xy(51.4775, -0.4850), xy(51.4777, -0.4332), 3)   # 09L / 27R (north)
line(xy(51.4647, -0.4826), xy(51.4649, -0.4340), 3)   # 09R / 27L (south)

def preview(path, S=6, home=(51.4579, -0.1985)):
    im = Image.new('RGB', (W * S, H * S), (5, 6, 8))
    d = ImageDraw.Draw(im)
    for y in range(H):
        for x in range(W):
            c = grid[y][x]
            if c:
                d.rectangle((x*S, y*S, x*S+S-2, y*S+S-2), fill=PAL[c])
    hx, hy = xy(*home)
    d.rectangle((int(hx)*S, int(hy)*S, int(hx)*S+S-2, int(hy)*S+S-2), fill=(255, 220, 120))
    im.save(path)

def emit():
    rows = []
    for y in range(H):
        rows.append('    "' + ''.join(str(c) for c in grid[y]) + '",')
    pal = ',\n'.join(f'    {{{r:3d}, {g:3d}, {b:3d}}}' for r, g, b in PAL)
    OUT.write_text(f'''// Generated by tools/map_workbench.py — do not edit by hand.
// London map base layer: 128x64, one palette digit per pixel, in flash.
#pragma once
#include <stdint.h>

namespace MapBase
{{
    // Bounding box (equirectangular): x = (lon - LON_W) / (LON_E - LON_W) * 128,
    // y = (LAT_N - lat) / (LAT_N - LAT_S) * 64.
    constexpr double LAT_N = {LAT_N}, LAT_S = {LAT_S};
    constexpr double LON_W = {LON_W}, LON_E = {LON_E};

    // 0 empty, 1 Thames, 2 reservoirs, 3 runways, 4 Heathrow apron, 5 M25,
    // 6 parks, 7 approach lanes
    static const uint8_t kPalette[][3] = {{
{pal}
    }};

    static const char *const kRows[64] = {{
{chr(10).join(rows)}
    }};
}}
''', encoding='utf-8')

if __name__ == '__main__':
    out = sys.argv[1] if len(sys.argv) > 1 else '.'
    preview(f'{out}/map_preview.png')
    emit()
    print('wrote', OUT.relative_to(ROOT))
