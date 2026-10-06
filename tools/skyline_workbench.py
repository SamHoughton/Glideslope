# London skyline for the landing scene: builds a 128x26 silhouette grid,
# renders a preview over the runway, and prints the C array.
#   '#' silhouette   'w' lit window   'r' aviation warning light
import sys
from PIL import Image, ImageDraw

W, H = 128, 26          # grid; bottom row sits on the runway edge (panel y 57)
g = [['.'] * W for _ in range(H)]

def px(x, y, c='#'):
    if 0 <= x < W and 0 <= y < H:
        g[y][x] = c

def rect(x0, y0, x1, y1, c='#'):            # inclusive, y counted from the top
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            px(x, y, c)

def ground(h):                               # helper: y of a roof h px tall
    return H - h

# Low rooftops along the whole horizon (deterministic "random" heights).
heights = [3, 4, 3, 5, 4, 3, 6, 4, 3, 5, 3, 4, 5, 3, 4, 6, 3, 4, 3, 5]
x = 0
for i in range(64):
    w = 3 + (i * 7) % 4
    h = heights[i % len(heights)]
    rect(x, ground(h), min(W - 1, x + w - 1), H - 1)
    x += w
    if x >= W:
        break

# Elizabeth Tower (Big Ben): shaft, clock stage, spire.
rect(5, ground(17), 7, H - 1)
rect(4, ground(14), 8, ground(11))
px(6, ground(13), 'w')                       # lit clock face
rect(5, ground(19), 7, ground(18))
px(6, ground(21)); px(6, ground(20))

# London Eye: rim, a few spokes, A-frame legs.
cx, cy, r = 24, ground(13), 9
import math
for a in range(0, 360, 6):
    px(round(cx + r * math.cos(math.radians(a))), round(cy + r * math.sin(math.radians(a))))
for a in range(0, 360, 45):
    for t in range(1, r):
        if t % 2 == 0:
            px(round(cx + t * math.cos(math.radians(a))), round(cy + t * math.sin(math.radians(a))))
px(cx, cy)
for t in range(0, 13):
    px(cx - 1 - t // 3, cy + t); px(cx + 1 + t // 3, cy + t)

# St Paul's Cathedral: drum, dome, lantern, cross.
rect(41, ground(9), 51, H - 1)
rect(43, ground(11), 49, ground(10))
for dx in range(-4, 5):
    hgt = round(math.sqrt(max(0, 16 - dx * dx)) * 0.9)
    for y in range(ground(11) - hgt, ground(11)):
        px(46 + dx, y)
rect(46, ground(17), 46, ground(15))
px(45, ground(16)); px(47, ground(16))

# The Gherkin: bullet shape.
for dy in range(0, 16):
    half = round(3.2 * math.sin(math.pi * min(1.0, (dy + 1) / 17) ** 0.8))
    rect(62 - half, ground(16) + dy, 62 + half, ground(16) + dy)
px(62, ground(17))
for dy in (4, 8, 12):
    px(61, ground(16) + dy, 'w'); px(63, ground(16) + dy + 1, 'w')

# Walkie-Talkie: widens towards the top.
for dy in range(0, 13):
    half = 3 + (12 - dy) // 5
    rect(72 - half, ground(13) + dy, 72 + half, ground(13) + dy)

# The Shard: tall tapering spire with a warning light.
for dy in range(0, 25):
    half = round(dy * 4.5 / 24)
    rect(86 - half, ground(25) + dy, 86 + half, ground(25) + dy)
px(86, ground(26), 'r')
for dy in (8, 12, 16, 20):
    px(85 + (dy // 4) % 2, ground(25) + dy, 'w')

# A couple of towers to the east, lower so the threshold stays clear.
rect(95, ground(11), 98, H - 1)
rect(101, ground(8), 103, H - 1)

# Scattered lit windows in the low buildings.
for x in range(1, W, 9):
    for y in range(H - 1, H - 6, -1):
        if g[y][x] == '#' and g[y - 1][x] == '#':
            g[y - 1][x] = 'w'
            break

rows = [''.join(r) for r in g]

def preview(path, S=6):
    pal = {'#': (32, 36, 50), 'w': (150, 110, 40), 'r': (230, 40, 40)}
    im = Image.new('RGB', (128 * S, 64 * S), (5, 6, 8))
    d = ImageDraw.Draw(im)
    def dot(x, y, c):
        d.ellipse((x*S+1, y*S+1, x*S+S-1, y*S+S-1), fill=c)
    top = 57 - H + 1
    for y, row in enumerate(rows):
        for x, c in enumerate(row):
            if c != '.':
                dot(x, top + y, pal[c])
    for y in range(58, 62):                       # runway surface
        for x in range(0, 109):
            dot(x, y, (26, 28, 34))
    for x in range(100, 109, 2):                  # piano keys
        for y in range(58, 62):
            dot(x, y, (215, 215, 220))
    im.save(path)

if __name__ == '__main__':
    out = sys.argv[1] if len(sys.argv) > 1 else '.'
    preview(f'{out}/skyline.png')
    if '--emit' in sys.argv:
        print('    const char *const kSkyline[] = {')
        for r in rows:
            print(f'        "{r}",')
        print('    };')
    print('ok')
