# Aircraft sprites, drawn from each type's proportions rather than by hand,
# so every type comes in two consistent sizes: the card / fly-across size
# (up to 44x16) and a larger one for the landing and take-off scenes (up to
# 60x22, with gear down). Liveries colour the result.
#
#   python tools/sprite_gen.py sheet out.png      -> preview sheet
#   python tools/sprite_gen.py emit               -> C arrays for AircraftSprites.cpp
#
# Characters (nose to the left):
#   '#' upper fuselage (livery body)    '-' cheatline (livery stripe)
#   '+' lower fuselage (livery belly)   'w' cabin windows   'c' cockpit glass
#   'r' fin (livery tail)               'R' fin accent (livery tail accent)
#   'g' wing / tailplane                'G' wing underside, flap
#   'e' engine cowl (livery engine)     'd' engine rear, gear, dark parts
#   'p' propeller blur
import math
import sys
from PIL import Image, ImageDraw

# ── Types ──────────────────────────────────────────────────────────────────
# L: length (px at card size), H: fuselage height, fin: fin height above the
# fuselage, nose: 'airbus' | 'boeing' | 'pointed' | 'blunt', eng: list of
# (kind, x as a fraction of the length, size), wing: 'low' | 'high',
# hump: 747 upper deck (fraction of length) | 0, deck2: A380 full upper deck,
# ttail: fin-top tailplane, props: propellers on the engines.
T = {
    'A320':        dict(L=36, H=6, fin=6, nose='airbus',  eng=[('wing', .38, 3)]),
    'A321':        dict(L=40, H=6, fin=6, nose='airbus',  eng=[('wing', .38, 3)]),
    '737':         dict(L=37, H=6, fin=7, nose='boeing',  eng=[('wing', .37, 3)], flat_eng=True),
    'A220':        dict(L=32, H=5, fin=6, nose='pointed', eng=[('wing', .38, 3)]),
    'E-Jet':       dict(L=31, H=5, fin=6, nose='pointed', eng=[('wing', .38, 3)]),
    'CRJ':         dict(L=30, H=4, fin=6, nose='pointed', eng=[('rear', .74, 3)], ttail=True),
    '757':         dict(L=42, H=6, fin=7, nose='boeing',  eng=[('wing', .34, 3)]),
    '767':         dict(L=40, H=7, fin=7, nose='boeing',  eng=[('wing', .36, 4)]),
    '787':         dict(L=42, H=7, fin=8, nose='smooth',  eng=[('wing', .36, 4)]),
    'A330':        dict(L=42, H=7, fin=8, nose='airbus',  eng=[('wing', .36, 4)]),
    'A350':        dict(L=44, H=8, fin=8, nose='smooth',  eng=[('wing', .35, 4)], mask=True),
    '777':         dict(L=44, H=8, fin=8, nose='boeing',  eng=[('wing', .35, 5)], blade=True),
    'A340':        dict(L=44, H=7, fin=8, nose='airbus',  eng=[('wing', .30, 3), ('wing', .45, 3)]),
    '747':         dict(L=44, H=8, fin=8, nose='boeing',  eng=[('wing', .32, 3), ('wing', .46, 3)], hump=.38),
    'A380':        dict(L=44, H=10, fin=8, nose='airbus', eng=[('wing', .32, 3), ('wing', .46, 3)], deck2=True),
    'ATR':         dict(L=30, H=5, fin=6, nose='blunt',   eng=[('wing', .36, 3)], wing='high', props=True, ttail=True),
    'Q400':        dict(L=33, H=5, fin=7, nose='pointed', eng=[('wing', .36, 3)], wing='high', props=True, ttail=True),
    'Bizjet':      dict(L=25, H=4, fin=5, nose='pointed', eng=[('rear', .70, 3)], ttail=True),
}

def build(t, scale=1.0, gear=False):
    """Character grid for type t. The fuselage is drawn from a smooth outline
    with sub-pixel coverage: pixels the outline only partly covers become the
    dim edge characters, which reads as a curve on the LEDs."""
    p = dict(T[t])
    L = round(p['L'] * scale)
    H = max(4, round(p['H'] * scale))
    fin = round(p['fin'] * scale)
    W, Hh = L + 3, H + fin + 10
    g = [['.'] * W for _ in range(Hh)]
    def put(x, y, ch, over=True):
        if 0 <= x < W and 0 <= y < Hh and (over or g[y][x] == '.'):
            g[y][x] = ch
    def get(x, y):
        return g[y][x] if 0 <= x < W and 0 <= y < Hh else '.'

    top0 = fin + 1.0                     # fuselage top edge (continuous)
    bot0 = top0 + H                      # fuselage bottom edge
    # Nose about as long as the fuselage is tall: round, not pointed.
    nl = H * {'airbus': 1.05, 'boeing': 1.2, 'pointed': 1.35, 'smooth': 1.15, 'blunt': 0.9}[p['nose']]
    tip = {'airbus': .56, 'boeing': .6, 'pointed': .6, 'smooth': .55, 'blunt': .55}[p['nose']]   # tip height (0 top, 1 bottom)
    tl = L * .22                         # tail cone length
    hump = p.get('hump', 0)

    def outline(x):
        """(top, bottom) of the fuselage at continuous x."""
        top, bot = top0, bot0
        if x < nl:
            t = (nl - x) / nl            # 1 at the tip
            k = 1 - math.sqrt(max(0.0, 1 - t ** 2.4))   # flat-ish then curving in
            tipy = top0 + H * tip
            top = top0 + (tipy - top0) * k
            bot = bot0 - (bot0 - tipy) * k
        if x > L - tl:
            u = (x - (L - tl)) / tl
            # Upswept underside to a blunt tail cone about a third of the height.
            bot = bot0 - (H * .62) * (u ** 1.3)
            top = top0 + (0.2 if p.get('blade') else H * .08) * u * u
        if hump:
            hx = L * hump
            lift = 2.2 * scale
            if x < nl * 0.6:   r = 0
            elif x < nl * 1.6: r = lift * math.sin((x - nl * .6) / nl * math.pi / 2)
            elif x < hx:       r = lift
            elif x < hx + 4 * scale: r = lift * (1 - (x - hx) / (4 * scale)) ** 1.5
            else:              r = 0
            top = min(top, top0 - r) if x > nl * .6 else top
        return top, max(top + 0.5, bot)

    # Coverage of each pixel by the fuselage (4x4 samples).
    S = 4
    cov = [[0.0] * W for _ in range(Hh)]
    for x in range(L):
        for sx in range(S):
            xc = x + (sx + .5) / S
            if xc > L: continue
            top, bot = outline(xc)
            for y in range(int(top) - 1, int(bot) + 2):
                for sy in range(S):
                    yc = y + (sy + .5) / S
                    if top <= yc <= bot and 0 <= y < Hh:
                        cov[y][x] += 1.0 / (S * S)
    # Shade by height within the column: highlight, body, cheatline, belly, shadow.
    for x in range(L):
        top, bot = outline(x + .5)
        for y in range(Hh):
            c = cov[y][x]
            if c < 0.28: continue
            rel = ((y + .5) - top) / max(0.5, bot - top)
            edge = c < 0.72
            if rel < 0.17:   ch = '^'
            elif rel < 0.55: ch = '#'
            elif rel < 0.68: ch = '-'
            elif rel < 0.86: ch = '+'
            else:            ch = '_'
            if edge: ch = 'a' if ch in '^#-' else 'b'
            put(x, y, ch)

    iy = lambda v: int(round(v))
    # Cabin windows, from behind the cockpit to the tail cone.
    win_y = iy(top0 + H * 0.3)
    for x in range(int(nl) + 2, int(L - tl) + 1):
        if (x - int(nl)) % 2 == 0 and get(x, win_y) in '#^': put(x, win_y, 'w')
    if p.get('deck2'):
        for x in range(int(nl) + 2, int(L - tl) - 1):
            if x % 2 == 1 and get(x, iy(top0 + 1)) in '#^': put(x, iy(top0 + 1), 'w')
    if hump:
        for x in range(int(nl * 1.6), int(L * hump) - 1):
            if x % 2 == 0 and get(x, iy(top0 - 1)) in '#^a': put(x, iy(top0 - 1), 'w')
    # Cockpit glass: on the upper curve of the nose.
    for dx in range(3 if p.get('mask') else 2):
        x = int(nl * 0.55) + dx
        top, _ = outline(x + .5)
        put(x, iy(top + .6) if not hump else iy(top + .6), 'c')

    # Fin: leading edge swept back, trailing edge nearly upright; the rear
    # strip in shade, the upper rear in the accent.
    fb = L * .17
    fx0 = L - fb - .5
    for y in range(iy(top0) - fin, iy(top0) + 1):
        i = iy(top0) - y                       # height above the fuselage top
        x0 = fx0 + i * 0.95
        x1 = L - 0.5 + i * 0.25
        for x in range(int(x0), int(x1) + 1):
            frac = (x - x0) / max(1, x1 - x0)
            if x + 1 - x0 < 0.5: continue
            ch = 'R' if (i > fin * .45 and frac > .3) else ('t' if frac > .78 else 'r')
            if x < x0: ch = 't'               # soft leading edge
            put(x, y, ch)
    # Tailplane.
    if p.get('ttail'):
        ty = iy(top0) - fin
        for x in range(int(L - fb * .5), L + 1): put(x, ty, 'g')
        put(L + 1, ty, 'G')
    else:
        # Tailplane: seen side on, a short root on the tail cone that just
        # shows past the end, not a long spike.
        tt, tb = outline(L - tl * .4)
        ty = iy(tt + (tb - tt) * .45)
        for x in range(int(L - tl * .55), L): put(x, ty, 'g')
        put(L, ty, 'G')

    # Wing: the fairing bulge under the belly, the wing sweeping back below it.
    w0, w1 = int(L * .36), int(L * .60)
    if p.get('wing') == 'high':
        for x in range(w0 - 2, w1 + 2): put(x, iy(top0) - 1, 'g')
        for x in range(w0, w1): put(x, iy(top0), 'G')
    else:
        for x in range(w0 - 1, w1 + 2):
            _, bot = outline(x + .5)
            yb = iy(bot)
            put(x, yb, '_' if w0 < x < w1 else 'b')                 # fairing
            if w0 + 1 <= x <= w1 - 1:
                put(x, yb + 1, 'g' if x < (w0 + w1) // 2 else 'G')  # wing, darker aft

    # Engines: intake ring, cowl lit on top and shaded underneath, exhaust.
    for kind, fx, size in p['eng']:
        es = max(2, round(size * scale))
        ex = int(L * fx)
        if kind == 'wing' and p.get('wing') == 'high':
            ey = iy(top0) - 1
            for x in range(ex - 1, ex + es + 1):
                put(x, ey, 'e'); put(x, ey + 1, 'E')
            put(ex - 1, ey, 'i'); put(ex - 1, ey + 1, 'i')
            if p.get('props'):
                for y in range(ey - 3, ey + 4): put(ex - 3, y, 'p')
        elif kind == 'wing':
            _, bot = outline(ex + 2.5)
            ey = iy(bot) + 1
            eh = max(2, round(es * .8))
            elen = es + 2
            for i in range(elen):
                x = ex + i
                lip = 1 if (i == 0 and eh > 2) else 0
                for y in range(ey + lip, ey + eh - lip):
                    rely = (y - ey) / max(1, eh - 1)
                    ch = 'i' if i == 0 else ('E' if rely > .6 else 'e')
                    put(x, y, ch)
            # exhaust cone
            for y in range(ey + 1, ey + eh - (1 if eh > 2 else 0)): put(ex + elen, y, 'i')
            put(ex + elen // 2 + 1, ey - 1, 'G', over=False)       # pylon
        else:   # rear-mounted
            tt, _ = outline(ex + .5)
            ey = iy(tt) - 1
            for i in range(es + 1):
                put(ex + i, ey, 'i' if i == 0 else 'e')
                put(ex + i, ey + 1, 'i' if i == 0 else 'E')
            put(ex + es + 1, ey, 'i')

    # Gear for the scenes: nose leg and the main bogie, wheels dark.
    if gear:
        _, nb = outline(nl + 1)
        _, mb = outline(L * .5)
        for gx, gb, legs in ((iy(nl) + 1, iy(nb), 3), (iy(L * .5), iy(mb) + 1, 3)):
            for i in range(legs): put(gx, gb + i, 'G')
            for dx in (-1, 0, 1): put(gx + dx, gb + legs, 'd')
            if gx > nl + 3:
                for dx in (2, 3, 4): put(gx + dx - 3 + 4, gb + legs, 'd')

    rows = [''.join(r) for r in g]
    while rows and set(rows[0]) == {'.'}: rows.pop(0)
    while rows and set(rows[-1]) == {'.'}: rows.pop()
    w = max(len(r.rstrip('.')) for r in rows)
    return [r[:w] for r in rows]

# ── Liveries (approximate colours, no logos) ───────────────────────────────
# code: (body, belly, stripe or None, tail, tail accent, engine)
WHITE, LGREY = (215, 220, 228), (150, 156, 168)
LIV = {
    'default': (WHITE, LGREY, None, (90, 170, 255), (50, 100, 160), (175, 180, 190)),
    'BAW': (WHITE, (28, 42, 95), (200, 30, 45), (28, 48, 110), (200, 30, 45), (28, 42, 95)),
    'VIR': ((210, 214, 222), (185, 190, 200), None, (210, 20, 40), (235, 235, 240), (210, 20, 40)),
    'EZY': (WHITE, LGREY, None, (255, 100, 0), (255, 255, 255), (255, 100, 0)),
    'RYR': (WHITE, (12, 35, 95), (245, 200, 0), (12, 35, 95), (245, 200, 0), (12, 35, 95)),
    'DLH': (WHITE, (200, 205, 215), None, (10, 30, 70), (225, 228, 235), (10, 30, 70)),
    'KLM': ((0, 155, 220), (215, 220, 228), (255, 255, 255), (0, 155, 220), (255, 255, 255), (175, 180, 190)),
    'AFR': (WHITE, (200, 205, 215), None, (0, 40, 130), (220, 30, 40), (175, 180, 190)),
    'UAE': (WHITE, (200, 205, 215), None, (210, 20, 30), (0, 125, 60), (215, 220, 228)),
    'QTR': ((215, 215, 222), (180, 182, 190), None, (110, 20, 60), (215, 215, 222), (110, 20, 60)),
    'ETD': ((215, 200, 170), (180, 165, 135), None, (170, 140, 80), (110, 90, 60), (215, 200, 170)),
    'AAL': ((180, 186, 196), (150, 156, 168), (190, 20, 40), (20, 50, 120), (200, 20, 40), (150, 156, 168)),
    'UAL': (WHITE, (20, 60, 145), None, (10, 40, 110), (90, 150, 220), (20, 60, 145)),
    'DAL': (WHITE, (8, 28, 75), None, (200, 15, 45), (8, 28, 75), (8, 28, 75)),
    'IBE': (WHITE, (200, 205, 215), None, (210, 15, 30), (255, 195, 0), (175, 180, 190)),
    'SWR': (WHITE, (200, 205, 215), None, (225, 0, 20), (255, 255, 255), (175, 180, 190)),
    'AUA': (WHITE, (215, 0, 30), None, (215, 0, 30), (255, 255, 255), (175, 180, 190)),
    'SAS': ((205, 210, 218), (175, 180, 190), None, (0, 30, 90), (205, 210, 218), (0, 30, 90)),
    'FIN': (WHITE, (200, 205, 215), None, (0, 45, 125), (255, 255, 255), (0, 45, 125)),
    'TAP': (WHITE, (200, 205, 215), None, (0, 140, 90), (220, 30, 40), (175, 180, 190)),
    'AEE': (WHITE, (200, 205, 215), None, (0, 60, 140), (110, 170, 230), (175, 180, 190)),
    'AIC': (WHITE, (200, 205, 215), None, (170, 0, 60), (200, 150, 50), (175, 180, 190)),
    'SIA': (WHITE, (200, 205, 215), (20, 40, 90), (20, 40, 90), (255, 185, 40), (175, 180, 190)),
    'CPA': (WHITE, (170, 175, 185), None, (0, 100, 80), (255, 255, 255), (175, 180, 190)),
    'ANA': (WHITE, (200, 205, 215), (0, 60, 145), (0, 40, 110), (100, 180, 230), (175, 180, 190)),
    'JAL': (WHITE, (200, 205, 215), None, (230, 232, 238), (230, 0, 20), (175, 180, 190)),
    'ACA': (WHITE, (25, 25, 30), None, (25, 25, 30), (215, 25, 30), (25, 25, 30)),
    'THY': (WHITE, (200, 205, 215), None, (200, 20, 30), (255, 255, 255), (175, 180, 190)),
    'ELY': (WHITE, (200, 205, 215), (0, 35, 100), (0, 35, 100), (255, 255, 255), (175, 180, 190)),
    'ICE': (WHITE, (200, 205, 215), None, (0, 30, 80), (240, 190, 40), (0, 30, 80)),
    'EIN': ((200, 228, 222), (215, 220, 228), None, (0, 120, 100), (130, 200, 190), (0, 120, 100)),
    'VLG': (WHITE, (150, 150, 155), None, (90, 90, 95), (255, 200, 0), (255, 200, 0)),
    'WZZ': (WHITE, (200, 205, 215), None, (195, 0, 110), (100, 30, 130), (195, 0, 110)),
    'TOM': (WHITE, (200, 205, 215), None, (110, 200, 240), (225, 30, 40), (110, 200, 240)),
    'KAL': ((160, 210, 235), (190, 195, 205), None, (230, 232, 238), (0, 80, 160), (175, 180, 190)),
    'CCA': (WHITE, (200, 205, 215), None, (230, 232, 238), (210, 20, 30), (175, 180, 190)),
    'MSR': (WHITE, (200, 205, 215), None, (20, 40, 90), (210, 170, 60), (175, 180, 190)),
    'RAM': (WHITE, (200, 205, 215), None, (200, 20, 40), (0, 120, 60), (175, 180, 190)),
    'SVA': ((220, 210, 185), (190, 180, 155), None, (0, 90, 150), (220, 210, 185), (175, 180, 190)),
    'GFA': (WHITE, (200, 205, 215), None, (190, 20, 40), (210, 170, 60), (175, 180, 190)),
    'OMA': (WHITE, (200, 205, 215), None, (0, 110, 120), (210, 170, 60), (175, 180, 190)),
    'ETH': (WHITE, (200, 205, 215), None, (0, 140, 70), (250, 200, 0), (175, 180, 190)),
    'PIA': (WHITE, (200, 205, 215), None, (0, 100, 60), (255, 255, 255), (175, 180, 190)),
    'FDX': (WHITE, (200, 205, 215), None, (77, 20, 140), (255, 100, 0), (175, 180, 190)),
    'UPS': ((90, 60, 40), (70, 48, 32), None, (90, 60, 40), (255, 180, 0), (90, 60, 40)),
    'LOT': (WHITE, (200, 205, 215), None, (0, 40, 100), (255, 255, 255), (175, 180, 190)),
    'BEL': (WHITE, (200, 205, 215), None, (0, 40, 100), (220, 30, 40), (175, 180, 190)),
}
ALIAS = {'SHT': 'BAW', 'CFE': 'BAW', 'EFW': 'BAW', 'EJU': 'EZY', 'EZS': 'EZY', 'RUK': 'RYR', 'LDM': 'RYR',
         'CLH': 'DLH', 'TFL': 'TOM', 'AMX': 'default'}
FIXED = {'w': (30, 40, 58), 'c': (18, 24, 40), 'g': (128, 135, 148), 'G': (80, 86, 100),
         'd': (50, 54, 62), 'p': (130, 135, 145), 'i': (35, 38, 46)}

def mixc(a, b, k):
    return tuple(int(a[i] + (b[i] - a[i]) * k) for i in range(3))

def sc(c, k):
    return tuple(int(v * k) for v in c)

def colour(ch, liv):
    """Livery colours with shading derived from them (the firmware does the same)."""
    body, belly, stripe, tail, acc, eng = liv
    stripe = stripe or body
    return {
        '^': mixc(body, (255, 255, 255), .35), '#': sc(body, .88), '-': sc(stripe, .85),
        '+': sc(belly, .78), '_': sc(belly, .55),
        'a': sc(body, .5), 'b': sc(belly, .42),
        'r': tail, 't': sc(tail, .62), 'R': acc,
        'e': mixc(eng, (255, 255, 255), .15), 'E': sc(eng, .6),
    }.get(ch) or FIXED[ch]

# ── Sheet ──────────────────────────────────────────────────────────────────
def draw(dr, rows, ox, oy, S, liv):
    for y, r in enumerate(rows):
        for x, ch in enumerate(r):
            if ch == '.': continue
            X, Y = (ox + x) * S, (oy + y) * S
            dr.ellipse((X + 1, Y + 1, X + S - 1, Y + S - 1), fill=colour(ch, liv))

def background(dr, x0, y0, w, h, S):
    for y in range(y0, y0 + h):
        for x in range(x0, x0 + w):
            dr.ellipse((x*S+S//2-1, y*S+S//2-1, x*S+S//2+1, y*S+S//2+1), fill=(22, 25, 31))

def sheet(path):
    S = 7
    names = list(T)
    cw, ch = 50, 20
    cols = 3
    sec1 = (len(names) + cols - 1) // cols
    show = ['BAW', 'VIR', 'EZY', 'RYR', 'KLM', 'UAE', 'DLH', 'AFR', 'QTR', 'AAL', 'DAL', 'ACA',
            'WZZ', 'SIA', 'IBE']
    livtypes = ['A320', '777', '787', 'A350', 'A321', '737', 'A380', '747', 'A330', 'A321', '767', 'A220',
                'A321', 'A350', 'A330']
    sec2 = (len(show) + cols - 1) // cols
    big = ['A320', '777', 'A380', 'ATR']
    sec3 = 2
    W, H = cols * cw, (sec1 + sec2) * ch + sec3 * 28 + 30
    im = Image.new('RGB', (W * S, H * S), (8, 9, 12))
    dr = ImageDraw.Draw(im)
    y = 0
    def label(x, yy, t):
        dr.text((x * S + 6, yy * S + 2), t, fill=(200, 200, 210))
    label(0, y, 'TYPES (card size, default livery)'); y += 2
    for k, n in enumerate(names):
        cx, cy = (k % cols) * cw, y + (k // cols) * ch
        background(dr, cx, cy, cw - 1, ch - 1, S)
        rows = build(n)
        draw(dr, rows, cx + (cw - len(rows[0])) // 2, cy + (ch - len(rows)) // 2, S, LIV['default'])
        label(cx, cy, f'{n} {len(rows[0])}x{len(rows)}')
    y += sec1 * ch + 1
    label(0, y, 'LIVERIES'); y += 2
    for k, code in enumerate(show):
        cx, cy = (k % cols) * cw, y + (k // cols) * ch
        background(dr, cx, cy, cw - 1, ch - 1, S)
        rows = build(livtypes[k])
        draw(dr, rows, cx + (cw - len(rows[0])) // 2, cy + (ch - len(rows)) // 2, S, LIV[code])
        label(cx, cy, f'{code} {livtypes[k]}')
    y += sec2 * ch + 1
    label(0, y, 'SCENE SIZE (gear down)'); y += 2
    for k, n in enumerate(big):
        cx, cy = (k % 2) * 75, y + (k // 2) * 28
        background(dr, cx, cy, 74, 27, S)
        rows = build(n, scale=1.35, gear=True)
        draw(dr, rows, cx + (75 - len(rows[0])) // 2, cy + (28 - len(rows)) // 2, S, LIV[['BAW', 'VIR', 'UAE', 'default'][k]])
        label(cx, cy, f'{n} {len(rows[0])}x{len(rows)}')
    # Old (hand drawn, before 4.0) against new, same airline colour.
    try:
        import importlib.util, os
        spec = importlib.util.spec_from_file_location('wb', os.path.join(os.path.dirname(__file__), 'sprite_workbench.py'))
        wb = importlib.util.module_from_spec(spec); spec.loader.exec_module(wb)
        old = {'w': (90, 170, 255), 'c': (40, 60, 90), '#': (235, 240, 245), '+': (165, 172, 185),
               'g': (125, 132, 145), 'd': (70, 75, 85), 'e': (190, 195, 205), 'p': (150, 155, 165)}
        pairs = [('Narrowbody', 'A320'), ('Widebody twin', '777'), ('747', '747')]
        H2 = 20
        im2 = Image.new('RGB', (im.width, im.height + (H2 * len(pairs) + 2) * S), (8, 9, 12))
        im2.paste(im, (0, 0))
        d2 = ImageDraw.Draw(im2)
        y0 = H + 1
        d2.text((6, y0 * S + 2), 'BEFORE / AFTER', fill=(200, 200, 210)); y0 += 2
        for k, (o, n) in enumerate(pairs):
            yy = y0 + k * H2
            background(d2, 0, yy, 74, H2 - 1, S); background(d2, 75, yy, 74, H2 - 1, S)
            rows = wb.BIG[o]
            for y, r in enumerate(rows):
                for x, ch in enumerate(r):
                    if ch == '.': continue
                    col = (28, 48, 110) if ch == 'r' else (14, 24, 55) if ch == 'R' else old[ch]
                    X, Y = (15 + x) * S, (yy + 3 + y) * S
                    d2.ellipse((X + 1, Y + 1, X + S - 1, Y + S - 1), fill=col)
            nr = build(n)
            draw(d2, nr, 75 + 15, yy + 2, S, LIV['BAW'])
        im = im2
    except Exception as e:
        print('no before/after:', e)
    im.save(path)

if __name__ == '__main__':
    if sys.argv[1:2] == ['sheet']:
        sheet(sys.argv[2])
        print('ok')

# ── Firmware tables ────────────────────────────────────────────────────────
# Kinds in firmware order (AircraftSprites::Kind), then the two hand-drawn ones.
KINDS = ['A320', 'A321', '737', 'A220', 'E-Jet', 'CRJ', '757', '767', '787', 'A330', 'A350', '777',
         'A340', '747', 'A380', 'ATR', 'Q400', 'Bizjet']
CARD_MAX, SCENE_MAX = (44, 17), (58, 26)

def fit(t, box, base, gear):
    s = base
    while s > 0.4:
        rows = build(t, scale=s, gear=gear)
        if len(rows[0]) <= box[0] and len(rows) <= box[1]:
            return rows
        s -= 0.03
    raise SystemExit(f'{t} does not fit {box}')

def hand_drawn():
    """Helicopter and light aircraft from the old workbench, in the new palette."""
    import importlib.util, os
    spec = importlib.util.spec_from_file_location('wb', os.path.join(os.path.dirname(__file__), 'sprite_workbench.py'))
    wb = importlib.util.module_from_spec(spec); spec.loader.exec_module(wb)
    wb.pad(wb.BIG)
    conv = lambda rows: [r.replace('R', 't').replace('e', 'e') for r in rows]
    return conv(wb.BIG['Helicopter']), conv(wb.BIG['Light aircraft'])

def c_ident(name):
    return ''.join(ch for ch in name.title() if ch.isalnum())

def emit_header(path):
    out = ['#pragma once',
           '// Generated by tools/sprite_gen.py: do not edit by hand.',
           '// Aircraft sprites (nose left) at card size and scene size (gear down),',
           '// and airline livery colours (approximate; colours only, no logos).',
           '#include <Arduino.h>', '', 'namespace SpriteData', '{']
    def arr(name, rows):
        w = max(len(r) for r in rows)
        out.append(f'    const char *const {name}[] = {{')
        for r in rows: out.append(f'        "{r.ljust(w, ".")}",')
        out.append('    };')
        return w, len(rows)
    card, scene = [], []
    for t in KINDS:
        cw, ch = arr(f'kCard{c_ident(t)}', fit(t, CARD_MAX, 1.0, False))
        sw, sh = arr(f'kScene{c_ident(t)}', fit(t, SCENE_MAX, 1.25, True))
        card.append((t, f'kCard{c_ident(t)}', cw, ch)); scene.append((t, f'kScene{c_ident(t)}', sw, sh))
    heli, light = hand_drawn()
    for nm, rows in (('Helicopter', heli), ('Light aircraft', light)):
        w, h = arr(f'kCard{c_ident(nm)}', rows)
        card.append((nm, f'kCard{c_ident(nm)}', w, h)); scene.append((nm, f'kCard{c_ident(nm)}', w, h))
    out.append('')
    out.append('    struct Entry { const char *name; uint8_t w, h; const char *const *rows; };')
    out.append('    const Entry kCard[] = {')
    for n, a, w, h in card: out.append(f'        {{"{n}", {w}, {h}, {a}}},')
    out.append('    };')
    out.append('    const Entry kScene[] = {')
    for n, a, w, h in scene: out.append(f'        {{"{n}", {w}, {h}, {a}}},')
    out.append('    };')
    out.append('')
    out.append('    // Airline code, then body, belly, stripe, tail, tail accent, engine (RGB);')
    out.append('    // hasStripe false: the cheatline takes the body colour.')
    out.append('    struct LiveryRow { char code[4]; uint8_t rgb[6][3]; bool hasStripe; };')
    out.append('    const LiveryRow kLiveries[] = {')
    for code, (body, belly, stripe, tail, acc, eng) in LIV.items():
        if code == 'default': continue
        cols = [body, belly, stripe or body, tail, acc, eng]
        s = ', '.join('{%d, %d, %d}' % c for c in cols)
        out.append(f'        {{"{code}", {{{s}}}, {"true" if stripe else "false"}}},')
    out.append('    };')
    out.append('    // Subsidiaries and brands flying the parent\'s colours.')
    out.append('    const char kAliases[][2][4] = {')
    for a, b in ALIAS.items():
        if b != 'default': out.append(f'        {{"{a}", "{b}"}},')
    out.append('    };')
    out.append('}')
    open(path, 'w', encoding='utf-8', newline='\n').write('\n'.join(out) + '\n')

if sys.argv[1:2] == ['emit']:
    emit_header(sys.argv[2] if len(sys.argv) > 2 else 'firmware/display/SpriteData.h')
    print('ok')
