# Sprite design workbench: renders previews and emits C arrays.
# Palette (nose left):
#   '#' fuselage   '+' belly shade   'w' windows   'c' cockpit glass
#   'r' tail / airline accent   'R' accent shade   'g' wing / stabiliser grey
#   'd' engine / gear dark   'e' engine cowl light   'p' propeller / rotor blur
import sys
from PIL import Image, ImageDraw

PAL = {
    '#': (235, 240, 245), '+': (165, 172, 185), 'w': (90, 170, 255), 'c': (40, 60, 90),
    'r': (230, 40, 60), 'R': (140, 20, 35), 'g': (125, 132, 145), 'd': (70, 75, 85),
    'e': (190, 195, 205), 'p': (150, 155, 165),
}

# ── Sprites: card (left column) and fly-across ───────────────────────────────────────────────
BIG = {
'Narrowbody': [
    "...............................rrr",
    "..............................rrRR",
    ".............................rrRRR",
    "............................rrRRRR",
    "...........................rrRRRRR",
    ".....######################rRRRRRR",
    "...##cc#w#w#w#w#w#w#w#w#w#w#w##RRRRggg",
    "..########################d######gggg",
    ".#################################+g",
    ".+++++++++++++++++++++++++++++++++",
    "...++++++gggggggg++++++++++++",
    "........ggggggggggg",
    ".........eeeeedd",
    ".........eeeeddd",
],
'Widebody twin': [
    "...................................rrr",
    "..................................rrRR",
    ".................................rrRRR",
    "................................rrRRRR",
    "...............................rrRRRRR",
    "..............................rrRRRRRR",
    "......########################rRRRRRRR",
    "....##cc#w#w#w#w#w#w#w#w#w#w#w#w#w##RRRRggg",
    "..#####################################gggg",
    ".######################################+g",
    "#######################################",
    ".++++++++++++++++++++++++++++++++++++",
    "....++++++gggggggggg++++++++++++++",
    "..........ggggggggggggg",
    ".........eeeeeeeddd",
    ".........eeeeeeedd",
],
'A380': [
    "...................................rrr",
    "..................................rrRR",
    ".................................rrRRR",
    "........########################rrRRRR",
    "......##w#w#w#w#w#w#w#w#w#w#w#w#rRRRRR",
    ".....###########################RRRRRR",
    "....##cc#w#w#w#w#w#w#w#w#w#w#w#w#w##RRRRggg",
    "..#####################################gggg",
    ".######################################+g",
    "#######################################",
    ".++++++++++++++++++++++++++++++++++++",
    "....++++gggggggggggggg+++++++++++",
    ".......eeeedd...eeeedd",
    ".......eeeedd...eeeedd",
],
'747': [
    "...................................rrr",
    "..................................rrRR",
    ".....#######.....................rrRRR",
    "....##cc#w#w##..................rrRRRR",
    "...############................rrRRRRR",
    "...#############################RRRRRR",
    "..###w#w#w#w#w#w#w#w#w#w#w#w#w#w##RRRRggg",
    ".######################################gggg",
    "#######################################+g",
    "#######################################",
    ".++++++++++++++++++++++++++++++++++++",
    "....++++gggggggggggggg+++++++++++",
    ".......eeeedd...eeeedd",
    ".......eeeedd...eeeedd",
],
'Regional jet': [
    "..........................rrr",
    ".........................rrRR",
    "........................rrRRR",
    ".......................rrRRRR",
    "....##################rrRRRRR",
    "..##cc#w#w#w#w#w#w#w#w##RRRRgg",
    ".############################g",
    ".##########################+",
    ".++++++++++++++++++++++++++",
    "...+++++ggggggg+++++++",
    "......ggggggggg",
    ".......eeeedd",
],
'Turboprop': [
    "..............................rr",
    ".............................rrR",
    ".....p...gggggggggggggggg...rrRR",
    ".....pgggeeeedd............rrRRR",
    ".....p...eeedd.............rRRRR",
    "....########################RRRRgg",
    "..##cc#w#w#w#w#w#w#w#w#w#w###RRgg",
    ".#############################+",
    ".###########################++",
    ".++++++++++++++++++++++++++",
    "........d.........",
],
'Business jet': [
    ".................rrrrrrrrr",
    "....................rrRR",
    "...................rrRR",
    "..................rRRR",
    "....###########eeeeddRR",
    "..##cc#w#w#w##eeeeddRR",
    ".################++++",
    ".##############+++",
    ".+++++++++++++",
    "....++ggggggg",
],
'Helicopter': [
    "ppppppppppppppppppppppppp",
    "...........dd",
    ".........######",
    "......##########",
    "....##ccw#########",
    "...#ccww############rrrrrrrrrrR",
    "...################+.......rrRR",
    "....++++++++++++++..........R",
    ".....d.........d",
    "...ddddddddddddddd",
],
'Light aircraft': [
    ".....gggggggggggggggg",
    "......#cc#",
    "p...#####c##..........r",
    "p.####################R",
    "p######################",
    "p...+++++++++++++.....gg",
    "......d.......d",
    ".....ddd.....ddd",
],
}

def pad(sprites):
    for name, rows in sprites.items():
        w = max(len(r) for r in rows)
        sprites[name] = [r.ljust(w, '.') for r in rows]

def check(name, rows):
    for i, r in enumerate(rows):
        for ch in r:
            if ch != '.' and ch not in PAL:
                sys.exit(f"{name}: bad char {ch!r}")

def render_set(sprites, path, S=10, cell=(36, 14)):
    cw, ch = cell
    names = list(sprites)
    cols = 3
    rows_n = (len(names) + cols - 1) // cols
    im = Image.new('RGB', (cols * cw * S, rows_n * ch * S), (5, 6, 8))
    dr = ImageDraw.Draw(im)
    for k, name in enumerate(names):
        rows = sprites[name]
        ox = (k % cols) * cw + (cw - len(rows[0])) // 2
        oy = (k // cols) * ch + (ch - len(rows)) // 2
        for y in range((k // cols) * ch, (k // cols + 1) * ch):
            for x in range((k % cols) * cw, (k % cols + 1) * cw):
                dr.ellipse((x*S+S//2-1, y*S+S//2-1, x*S+S//2+1, y*S+S//2+1), fill=(22, 25, 31))
        for y, r in enumerate(rows):
            for x, c in enumerate(r):
                if c == '.': continue
                X, Y = (ox + x) * S, (oy + y) * S
                dr.ellipse((X+1, Y+1, X+S-1, Y+S-1), fill=PAL[c])
    im.save(path)

def emit(sprites, prefix):
    out = []
    for name, rows in sprites.items():
        ident = prefix + ''.join(p.capitalize() for p in name.replace('-', ' ').split())
        out.append(f"    const char *const {ident}[] = {{")
        for r in rows:
            out.append(f'        "{r.replace(" ", ".")}",')
        out.append("    };")
    return "\n".join(out)

if __name__ == '__main__':
    pad(BIG)
    for n, r in BIG.items(): check(n, r)
    here = sys.argv[1] if len(sys.argv) > 1 else '.'
    render_set(BIG, f'{here}/sprites_big.png', S=8, cell=(48, 18))
    if '--emit' in sys.argv:
        print(emit(BIG, 'kBig'))
    print('ok')
