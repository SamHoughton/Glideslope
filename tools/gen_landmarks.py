#!/usr/bin/env python3
"""
City landmarks as silhouettes for the panel's origin page (9.1): the page
shows only for an origin that has one. Each is drawn here at four times the
size with simple shapes, scaled down to at most 50x44 panel pixels, and
written to firmware/display/Landmarks.{h,cpp} as 1-bit bitmaps, with the
airports (IATA) that serve the city.

  python tools/gen_landmarks.py            write the firmware files
  python tools/gen_landmarks.py --sheet F  also a contact sheet (PNG) to check them

Original drawings of public buildings and places; no logos or marks.
"""
import argparse
import math
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
W, H = 50, 44          # the most a landmark may take on the panel
S = 4                  # drawn at four times the size


class Pen:
    def __init__(self):
        self.img = Image.new('L', (W * S, H * S), 0)
        self.d = ImageDraw.Draw(self.img)

    def _p(self, pts):
        return [(x * S, y * S) for x, y in pts]

    def poly(self, *pts):
        self.d.polygon(self._p(pts), fill=255)

    def rect(self, x0, y0, x1, y1):
        self.d.rectangle((x0 * S, y0 * S, x1 * S - 1, y1 * S - 1), fill=255)

    def ellipse(self, x0, y0, x1, y1, fill=255):
        self.d.ellipse((x0 * S, y0 * S, x1 * S, y1 * S), fill=fill)

    def line(self, x0, y0, x1, y1, w=1.0):
        self.d.line((x0 * S, y0 * S, x1 * S, y1 * S), fill=255, width=max(1, int(w * S)))

    def cut_rect(self, x0, y0, x1, y1):
        self.d.rectangle((x0 * S, y0 * S, x1 * S - 1, y1 * S - 1), fill=0)

    def cut_ellipse(self, x0, y0, x1, y1):
        self.d.ellipse((x0 * S, y0 * S, x1 * S, y1 * S), fill=0)

    def cut_poly(self, *pts):
        self.d.polygon(self._p(pts), fill=0)

    def bits(self):
        small = self.img.resize((W, H), Image.BOX)
        return [[1 if small.getpixel((x, y)) >= 110 else 0 for x in range(W)] for y in range(H)]


G = H  # the ground line


def eiffel(p):
    cx = 25
    p.poly((cx - 15, G), (cx - 9, G), (cx - 3.2, G - 26), (cx + 3.2, G - 26), (cx + 9, G), (cx + 15, G))
    p.cut_ellipse(cx - 8, G - 9, cx + 8, G + 6)           # the arch between the legs
    p.rect(cx - 11, G - 13, cx + 11, G - 11)              # first platform
    p.rect(cx - 5.5, G - 27, cx + 5.5, G - 25.5)          # second
    p.poly((cx - 3, G - 26), (cx + 3, G - 26), (cx + 1, G - 41), (cx - 1, G - 41))
    p.rect(cx - 1.6, G - 42, cx + 1.6, G - 40)
    p.line(cx, G - 42, cx, G - 44, 0.7)


def burj(p):
    cx = 25
    tiers = [(9, 0), (7.5, 9), (6, 17), (4.6, 24), (3.4, 30), (2.4, 35), (1.5, 39)]
    for i, (half, y) in enumerate(tiers):
        top = tiers[i + 1][1] if i + 1 < len(tiers) else 41
        p.rect(cx - half, G - top, cx + half, G - y)
    p.line(cx, G - 41, cx, G - 44, 0.8)
    p.rect(cx - 13, G - 3, cx + 13, G)


def empire(p):
    cx = 25
    p.rect(cx - 11, G - 14, cx + 11, G)
    p.rect(cx - 8, G - 28, cx + 8, G - 14)
    p.rect(cx - 6, G - 31, cx + 6, G - 28)
    p.rect(cx - 4.2, G - 34, cx + 4.2, G - 31)
    p.rect(cx - 2.6, G - 37, cx + 2.6, G - 34)
    p.poly((cx - 1.6, G - 37), (cx + 1.6, G - 37), (cx + 0.6, G - 41), (cx - 0.6, G - 41))
    p.line(cx, G - 41, cx, G - 44, 0.6)
    p.rect(1, G - 9, 9, G)                                 # neighbours either side
    p.rect(40, G - 12, 48, G)


def marina_bay(p):
    for x in (9, 21, 33):
        p.poly((x, G), (x + 8, G), (x + 7, G - 26), (x + 1, G - 26))
    p.poly((5, G - 26), (46, G - 26), (48, G - 29), (5, G - 29))      # the boat on top
    p.poly((46, G - 29), (49, G - 29), (47, G - 27))


def opera(p):
    p.rect(2, G - 4, 48, G)
    shells = [(4, 22, 18), (13, 30, 24), (24, 40, 20), (33, 46, 14)]
    for x0, x1, h in shells:
        p.poly((x0, G - 4), (x1, G - 4), (x1 - (x1 - x0) * 0.15, G - 4 - h * 0.35), (x0 + (x1 - x0) * 0.25, G - 4 - h))
        p.cut_poly((x1 - 1.2, G - 4), (x1, G - 4), (x1 - (x1 - x0) * 0.15 - 1, G - 4 - h * 0.35 - 0.5))


def cn_tower(p):
    cx = 25
    p.poly((cx - 5, G), (cx + 5, G), (cx + 1.6, G - 26), (cx - 1.6, G - 26))
    p.ellipse(cx - 5, G - 30, cx + 5, G - 25)
    p.rect(cx - 1, G - 37, cx + 1, G - 29)
    p.ellipse(cx - 2, G - 37, cx + 2, G - 34)
    p.line(cx, G - 37, cx, G - 44, 0.6)
    for x, h in ((3, 10), (10, 14), (36, 12), (43, 8)):
        p.rect(x, G - h, x + 6, G)


def space_needle(p):
    cx = 25
    p.poly((cx - 7, G), (cx - 4, G), (cx - 1, G - 22), (cx - 2.5, G - 22))
    p.poly((cx + 7, G), (cx + 4, G), (cx + 1, G - 22), (cx + 2.5, G - 22))
    p.rect(cx - 1.2, G - 22, cx + 1.2, G - 12)
    p.poly((cx - 12, G - 26), (cx + 12, G - 26), (cx + 5, G - 22), (cx - 5, G - 22))
    p.poly((cx - 9, G - 26), (cx + 9, G - 26), (cx + 5, G - 29), (cx - 5, G - 29))
    p.rect(cx - 0.7, G - 37, cx + 0.7, G - 29)
    p.rect(cx - 5, G - 12, cx + 5, G - 11)


def golden_gate(p):
    p.rect(0, G - 4, 50, G - 3)                            # the deck
    for x in (12, 36):
        p.rect(x - 2, G - 34, x - 0.8, G)
        p.rect(x + 0.8, G - 34, x + 2, G)
        for y in (G - 33, G - 25, G - 17, G - 10):
            p.rect(x - 2, y, x + 2, y + 1.2)
    for x0, x1, sag in ((-14, 12, 1), (12, 36, 1), (36, 62, 1)):   # the main cables
        pts = []
        for i in range(21):
            t = i / 20
            x = x0 + (x1 - x0) * t
            y = (G - 33) + 26 * (4 * t * (1 - t)) * sag
            if x0 < 0 and t < 0.5: y = G - 4 - (G - 37 - (G - 4)) * (t / 0.5) ** 2 if False else y
            pts.append((x, y))
        for (ax, ay), (bx, by) in zip(pts, pts[1:]):
            p.line(ax, ay, bx, by, 0.6)


def petronas(p):
    for cx in (17, 33):
        tiers = [(5, 0), (4.2, 14), (3.4, 22), (2.6, 28), (1.6, 33)]
        for i, (half, y) in enumerate(tiers):
            top = tiers[i + 1][1] if i + 1 < len(tiers) else 36
            p.rect(cx - half, G - top, cx + half, G - y)
        p.line(cx, G - 36, cx, G - 44, 0.7)
    p.rect(17, G - 19, 33, G - 17.5)                       # the skybridge


def tokyo_tower(p):
    cx = 25
    p.poly((cx - 12, G), (cx - 7, G), (cx - 2.4, G - 24), (cx + 2.4, G - 24), (cx + 7, G), (cx + 12, G))
    p.cut_ellipse(cx - 6, G - 7, cx + 6, G + 5)
    p.rect(cx - 8, G - 12, cx + 8, G - 10)
    p.rect(cx - 4, G - 25, cx + 4, G - 23)
    p.poly((cx - 2.2, G - 24), (cx + 2.2, G - 24), (cx + 0.8, G - 38), (cx - 0.8, G - 38))
    p.line(cx, G - 38, cx, G - 43, 0.6)


def india_gate(p):
    cx = 25
    p.rect(cx - 12, G - 26, cx + 12, G)
    p.cut_ellipse(cx - 6, G - 22, cx + 6, G - 8)
    p.cut_rect(cx - 6, G - 15, cx + 6, G)
    p.rect(cx - 13.5, G - 29, cx + 13.5, G - 26)
    p.rect(cx - 9, G - 32, cx + 9, G - 29)
    p.ellipse(cx - 5, G - 36, cx + 5, G - 30)
    p.rect(cx - 15, G - 2, cx + 15, G)


def gateway_india(p):
    cx = 25
    p.rect(cx - 14, G - 22, cx + 14, G)
    p.cut_ellipse(cx - 5, G - 19, cx + 5, G - 8)
    p.cut_rect(cx - 5, G - 13, cx + 5, G)
    for x in (cx - 13, cx + 9):
        p.rect(x, G - 30, x + 4, G - 22)
        p.ellipse(x - 0.5, G - 33, x + 4.5, G - 28)
    p.rect(cx - 5, G - 26, cx + 5, G - 22)
    p.ellipse(cx - 4, G - 30, cx + 4, G - 24)


def colosseum(p):
    p.poly((3, G), (47, G), (47, G - 18), (30, G - 24), (3, G - 22))
    for row, (y0, y1) in enumerate(((G - 5, G - 1), (G - 11, G - 7), (G - 17, G - 13))):
        for x in range(5 + row % 2, 46, 4):
            if row == 2 and x > 32: continue
            p.cut_rect(x, y0, x + 2, y1)
    p.cut_poly((36, G - 25), (48, G - 25), (48, G - 19))


def parthenon(p):
    p.poly((0, G), (50, G), (50, G - 6), (0, G - 6))         # the rock
    p.rect(6, G - 9, 44, G - 6)
    for x in range(8, 43, 4):
        p.rect(x, G - 21, x + 2, G - 9)
    p.rect(6, G - 24, 44, G - 21)
    p.poly((6, G - 24), (44, G - 24), (25, G - 30))


def pyramids(p):
    p.poly((2, G), (30, G), (16, G - 26))
    p.poly((20, G), (48, G), (35, G - 22))
    p.poly((40, G), (50, G), (46, G - 7))


def mosque(p):
    cx = 25
    p.rect(cx - 13, G - 12, cx + 13, G)
    p.ellipse(cx - 9, G - 25, cx + 9, G - 7)
    p.rect(cx - 0.6, G - 29, cx + 0.6, G - 24)
    for x in (cx - 13, cx + 13):
        p.ellipse(x - 4, G - 16, x + 4, G - 8)
    for x in (4, 46):
        p.rect(x - 1.2, G - 34, x + 1.2, G)
        p.poly((x - 1.2, G - 34), (x + 1.2, G - 34), (x, G - 40))
        p.rect(x - 1.8, G - 24, x + 1.8, G - 23)


def tv_tower(p):
    cx = 25
    p.poly((cx - 3, G), (cx + 3, G), (cx + 1.4, G - 24), (cx - 1.4, G - 24))
    p.ellipse(cx - 5.5, G - 33, cx + 5.5, G - 22)
    p.rect(cx - 1, G - 38, cx + 1, G - 32)
    p.line(cx, G - 38, cx, G - 44, 0.6)
    for x, h in ((2, 8), (9, 11), (34, 10), (42, 7)):
        p.rect(x, G - h, x + 6, G)


def sagrada(p):
    p.rect(8, G - 12, 42, G)
    for x, h in ((10, 30), (16, 36), (31, 36), (37, 30), (23.5, 42)):
        w = 3 if h < 40 else 3.5
        p.poly((x - w, G - 12), (x + w, G - 12), (x + 0.8, G - h), (x - 0.8, G - h))
        p.ellipse(x - 1, G - h - 2, x + 1, G - h)


def windmill(p):
    cx = 25
    p.poly((cx - 7, G), (cx + 7, G), (cx + 4, G - 22), (cx - 4, G - 22))
    p.poly((cx - 5, G - 22), (cx + 5, G - 22), (cx, G - 28))
    hub = (cx, G - 22)
    for a in (35, 125, 215, 305):
        r = math.radians(a)
        ex, ey = hub[0] + 18 * math.cos(r), hub[1] + 18 * math.sin(r)
        nx, ny = -math.sin(r), math.cos(r)
        p.poly((hub[0], hub[1]), (ex, ey), (ex + nx * 4, ey + ny * 4), (hub[0] + nx * 1.5, hub[1] + ny * 1.5))
    for x in (2, 41):
        p.rect(x, G - 8, x + 7, G)
        p.poly((x, G - 8), (x + 7, G - 8), (x + 3.5, G - 12))


def palms(p):
    def palm(bx, h, lean):
        tx, ty = bx + lean, G - h
        prev = (bx, G)
        for i in range(1, 13):
            t = i / 12
            pt = (bx + lean * t * t, G - h * t)
            p.line(prev[0], prev[1], pt[0], pt[1], 1.6 - 0.5 * t)   # the trunk, thinning
            prev = pt
        for a, l in ((200, 9), (235, 8), (270, 6), (305, 8), (340, 9), (160, 7), (20, 7)):
            r = math.radians(a)
            ex, ey = tx + l * math.cos(r), ty + l * math.sin(r) + 3
            p.line(tx, ty, (tx + ex) / 2, ty + (ey - ty) / 2 - 1.5, 1.1)
            p.line((tx + ex) / 2, ty + (ey - ty) / 2 - 1.5, ex, ey, 0.9)
    palm(12, 36, 4)
    palm(30, 42, -3)
    palm(42, 26, 3)


def willis(p):
    tubes = [(13, 4, 30), (17, 4, 38), (21, 4, 41), (25, 4, 38), (29, 4, 33)]
    for x, w, h in tubes:
        p.rect(x, G - h, x + w, G)
    p.line(20, G - 41, 20, G - 44, 0.6)
    p.line(24, G - 41, 24, G - 44, 0.6)
    for x, h in ((2, 12), (38, 16), (44, 10)):
        p.rect(x, G - h, x + 6, G)


def capitol(p):
    cx = 25
    p.rect(2, G - 8, 48, G)
    p.rect(cx - 8, G - 12, cx + 8, G - 8)
    p.rect(cx - 6, G - 16, cx + 6, G - 12)
    p.ellipse(cx - 6, G - 26, cx + 6, G - 12)
    p.rect(cx - 1.2, G - 30, cx + 1.2, G - 25)
    p.ellipse(cx - 1.4, G - 32, cx + 1.4, G - 29)


def christ(p):
    cx = 25
    p.poly((0, G), (50, G), (40, G - 8), (30, G - 14), (20, G - 14), (8, G - 7))   # the mountain
    p.rect(cx - 2, G - 20, cx + 2, G - 14)
    p.poly((cx - 2.6, G - 20), (cx + 2.6, G - 20), (cx + 1.8, G - 32), (cx - 1.8, G - 32))
    p.rect(cx - 13, G - 31, cx + 13, G - 29)               # the arms
    p.ellipse(cx - 1.6, G - 36, cx + 1.6, G - 32)


def table_mountain(p):
    p.poly((0, G), (50, G), (50, G - 10), (44, G - 14), (38, G - 26), (12, G - 26), (8, G - 22), (3, G - 18), (0, G - 16))
    p.poly((40, G - 22), (46, G - 30), (50, G - 24), (50, G - 14))


def pearl_tower(p):
    cx = 25
    p.poly((cx - 8, G), (cx - 6, G), (cx - 1, G - 14), (cx - 2.5, G - 14))
    p.poly((cx + 8, G), (cx + 6, G), (cx + 1, G - 14), (cx + 2.5, G - 14))
    p.ellipse(cx - 6, G - 20, cx + 6, G - 9)
    p.rect(cx - 1.4, G - 33, cx + 1.4, G - 19)
    p.ellipse(cx - 3.5, G - 37, cx + 3.5, G - 30)
    p.line(cx, G - 37, cx, G - 44, 0.6)


def taipei101(p):
    cx = 25
    p.rect(cx - 6, G - 10, cx + 6, G)
    for i in range(7):
        y = G - 10 - i * 4
        p.poly((cx - 4.5, y), (cx + 4.5, y), (cx + 5.8, y - 4), (cx - 5.8, y - 4))
    p.rect(cx - 2.5, G - 41, cx + 2.5, G - 38)
    p.line(cx, G - 41, cx, G - 44, 0.6)


def temple_heaven(p):
    cx = 25
    p.rect(cx - 15, G - 4, cx + 15, G)
    p.rect(cx - 11, G - 10, cx + 11, G - 4)
    for i, (half, h) in enumerate(((14, 5), (11, 5), (8, 5))):
        y = G - 10 - i * 7
        p.poly((cx - half, y), (cx + half, y), (cx + half - 4, y - h), (cx - half + 4, y - h))
        p.rect(cx - half + 4, y - 7, cx + half - 4, y - h)
    p.ellipse(cx - 1.5, G - 34, cx + 1.5, G - 31)


def seoul_tower(p):
    p.poly((0, G), (50, G), (40, G - 8), (25, G - 12), (10, G - 8))
    cx = 25
    p.rect(cx - 1.5, G - 30, cx + 1.5, G - 12)
    p.rect(cx - 4, G - 33, cx + 4, G - 28)
    p.line(cx, G - 33, cx, G - 43, 0.6)


def kingdom_centre(p):
    cx = 25
    p.poly((cx - 8, G), (cx + 8, G), (cx + 8, G - 30), (cx + 3, G - 42), (cx - 3, G - 42), (cx - 8, G - 30))
    p.cut_ellipse(cx - 5, G - 44, cx + 5, G - 30)
    p.cut_rect(cx - 5, G - 44, cx + 5, G - 37)
    p.rect(cx - 8, G - 30, cx - 7, G - 28)
    p.rect(1, G - 8, 8, G)
    p.rect(42, G - 11, 49, G)


def riesenrad(p):
    cx, cy, r = 25, G - 21, 17
    p.ellipse(cx - r, cy - r, cx + r, cy + r)
    p.cut_ellipse(cx - r + 1.6, cy - r + 1.6, cx + r - 1.6, cy + r - 1.6)
    for a in range(0, 360, 30):
        rad = math.radians(a)
        p.line(cx, cy, cx + r * math.cos(rad), cy + r * math.sin(rad), 0.45)
        p.rect(cx + r * math.cos(rad) - 1.2, cy + r * math.sin(rad) - 1, cx + r * math.cos(rad) + 1.2, cy + r * math.sin(rad) + 1.4)
    p.poly((cx - 9, G), (cx - 6, G), (cx + 0.6, cy), (cx - 0.6, cy))
    p.poly((cx + 9, G), (cx + 6, G), (cx - 0.6, cy), (cx + 0.6, cy))


def jet_eau(p):
    p.poly((0, G), (50, G), (50, G - 14), (40, G - 22), (32, G - 16), (24, G - 26), (16, G - 18), (8, G - 22), (0, G - 14))   # the Alps
    p.cut_poly((0, G), (50, G), (50, G - 6), (0, G - 6))
    p.rect(0, G - 2, 50, G)
    cx = 25
    p.poly((cx - 0.8, G - 2), (cx + 0.8, G - 2), (cx + 1.2, G - 36), (cx - 1.2, G - 36))
    p.ellipse(cx - 3, G - 40, cx + 3, G - 33)


def castle(p):
    p.poly((0, G), (50, G), (44, G - 12), (36, G - 16), (10, G - 16), (4, G - 10))   # the rock
    p.rect(12, G - 26, 38, G - 16)
    for x in range(12, 38, 4):
        p.rect(x, G - 28, x + 2, G - 26)
    p.rect(30, G - 34, 36, G - 26)
    p.rect(14, G - 31, 19, G - 26)
    p.poly((30, G - 34), (36, G - 34), (33, G - 38))


def saguaro(p):
    p.rect(22, G - 34, 27, G)
    p.ellipse(22, G - 36, 27, G - 32)
    p.rect(14, G - 18, 22, G - 15)
    p.rect(14, G - 27, 17, G - 15)
    p.ellipse(14, G - 29, 17, G - 25)
    p.rect(27, G - 22, 34, G - 19)
    p.rect(31, G - 30, 34, G - 19)
    p.ellipse(31, G - 32, 34, G - 28)
    p.poly((0, G), (50, G), (50, G - 6), (38, G - 9), (8, G - 7), (0, G - 5))


def angel_mexico(p):
    cx = 25
    p.rect(cx - 8, G - 6, cx + 8, G)
    p.rect(cx - 5, G - 10, cx + 5, G - 6)
    p.rect(cx - 1.6, G - 34, cx + 1.6, G - 10)
    p.rect(cx - 2.4, G - 35, cx + 2.4, G - 33)
    p.poly((cx - 1, G - 35), (cx + 1, G - 35), (cx + 0.6, G - 40), (cx - 0.6, G - 40))
    p.poly((cx - 0.5, G - 39), (cx - 6, G - 43), (cx - 4, G - 38))
    p.poly((cx + 0.5, G - 39), (cx + 6, G - 43), (cx + 4, G - 38))


def belem(p):
    p.rect(0, G - 3, 50, G)
    p.rect(14, G - 14, 36, G - 3)
    p.rect(20, G - 30, 32, G - 14)
    for x in range(20, 32, 3):
        p.rect(x, G - 32, x + 1.6, G - 30)
    for x in (14, 34):
        p.rect(x, G - 17, x + 2, G - 14)
        p.ellipse(x - 0.4, G - 19, x + 2.4, G - 16)


def wat_arun(p):
    cx = 25
    p.rect(cx - 15, G - 5, cx + 15, G)
    p.poly((cx - 10, G - 5), (cx + 10, G - 5), (cx + 3, G - 30), (cx + 1, G - 38), (cx - 1, G - 38), (cx - 3, G - 30))
    for x in (cx - 15, cx + 15):
        p.poly((x - 4, G - 5), (x + 4, G - 5), (x + 1, G - 18), (x - 1, G - 18))
    p.line(cx, G - 38, cx, G - 43, 0.6)


def skyline_hk(p):
    blocks = [(0, 4, 14), (4, 5, 20), (9, 4, 26), (13, 6, 17), (19, 5, 30), (24, 4, 22), (34, 5, 25), (39, 5, 18), (44, 6, 12)]
    for x, w, h in blocks:
        p.rect(x, G - h, x + w, G)
    p.poly((28, G), (34, G), (34, G - 34), (31, G - 40), (28, G - 34))   # Bank of China, angular
    p.line(30.2, G - 40, 30.2, G - 44, 0.5)
    p.line(32.0, G - 40, 32.0, G - 44, 0.5)


def frankfurt(p):
    blocks = [(2, 5, 14), (8, 5, 30), (14, 6, 22), (21, 5, 36), (27, 6, 26), (34, 5, 40), (40, 5, 18), (45, 5, 12)]
    for x, w, h in blocks:
        p.rect(x, G - h, x + w, G)
    p.poly((34, G - 40), (39, G - 40), (36.5, G - 43))


def aspire(p):
    cx = 25
    p.poly((cx - 4, G), (cx + 4, G), (cx + 2.5, G - 30), (cx - 2.5, G - 30))
    p.ellipse(cx - 5, G - 34, cx + 5, G - 26)
    p.rect(cx - 1.8, G - 40, cx + 1.8, G - 33)
    p.line(cx, G - 40, cx, G - 44, 0.6)
    p.rect(2, G - 10, 9, G)
    p.rect(40, G - 14, 46, G)


# name, drawing, airports (IATA)
LANDMARKS = [
    ('Eiffel Tower', eiffel, 'CDG ORY'),
    ('Burj Khalifa', burj, 'DXB DWC'),
    ('Empire State', empire, 'JFK EWR LGA'),
    ('Marina Bay Sands', marina_bay, 'SIN'),
    ('Opera House', opera, 'SYD'),
    ('CN Tower', cn_tower, 'YYZ'),
    ('Space Needle', space_needle, 'SEA'),
    ('Golden Gate', golden_gate, 'SFO'),
    ('Petronas Towers', petronas, 'KUL'),
    ('Tokyo Tower', tokyo_tower, 'HND NRT'),
    ('India Gate', india_gate, 'DEL'),
    ('Gateway of India', gateway_india, 'BOM'),
    ('Colosseum', colosseum, 'FCO CIA'),
    ('Parthenon', parthenon, 'ATH'),
    ('Pyramids', pyramids, 'CAI'),
    ('Mosque', mosque, 'IST SAW AUH'),
    ('TV Tower', tv_tower, 'BER'),
    ('Sagrada Familia', sagrada, 'BCN'),
    ('Windmill', windmill, 'AMS'),
    ('Palms', palms, 'LAX MIA'),
    ('Willis Tower', willis, 'ORD'),
    ('Capitol', capitol, 'IAD DCA'),
    ('Christ the Redeemer', christ, 'GIG'),
    ('Table Mountain', table_mountain, 'CPT'),
    ('Oriental Pearl', pearl_tower, 'PVG SHA'),
    ('Taipei 101', taipei101, 'TPE'),
    ('Temple of Heaven', temple_heaven, 'PEK PKX'),
    ('N Seoul Tower', seoul_tower, 'ICN'),
    ('Kingdom Centre', kingdom_centre, 'RUH'),
    ('Riesenrad', riesenrad, 'VIE'),
    ('Jet d\'Eau', jet_eau, 'GVA'),
    ('Castle', castle, 'EDI'),
    ('Saguaro', saguaro, 'PHX'),
    ('Angel of Independence', angel_mexico, 'MEX'),
    ('Belem Tower', belem, 'LIS'),
    ('Wat Arun', wat_arun, 'BKK'),
    ('Hong Kong', skyline_hk, 'HKG'),
    ('Frankfurt', frankfurt, 'FRA'),
    ('Aspire Tower', aspire, 'DOH'),
]


def render_all():
    out = []
    for name, fn, airports in LANDMARKS:
        p = Pen()
        fn(p)
        out.append((name, airports.split(), p.bits()))
    return out


def write(items):
    rows_c = []
    for name, airports, bits in items:
        # Crop to the drawing (columns only; the ground stays at the bottom).
        cols = [x for x in range(W) if any(bits[y][x] for y in range(H))]
        top = next(y for y in range(H) if any(bits[y]))
        x0, x1 = min(cols), max(cols) + 1
        w, h = x1 - x0, H - top
        stride = (w + 7) // 8
        data = []
        for y in range(top, H):
            for b in range(stride):
                v = 0
                for k in range(8):
                    x = x0 + b * 8 + k
                    if x < x1 and bits[y][x]:
                        v |= 0x80 >> k
                data.append(v)
        rows_c.append((name, airports, w, h, data))
    h_out = '''#pragma once
/*
Purpose: City landmarks as 1-bit silhouettes (9.1), for the origin page:
it shows only for an origin that has one. Generated by tools/gen_landmarks.py
(drawn there with simple shapes, scaled down): do not edit by hand.
*/
#include <stdint.h>

namespace Landmarks
{
struct Landmark
{
    const char    *name;
    uint8_t        w, h;       // at most 50 x 44; the bottom row stands on the ground
    const uint8_t *bits;       // rows of (w + 7) / 8 bytes, high bit first
};
// The landmark for an airport (IATA), or nullptr.
const Landmark *forAirport(const char *iata);
}
'''
    c = ['// Generated by tools/gen_landmarks.py - do not edit by hand.',
         '#include "display/Landmarks.h"', '#include <string.h>', '', 'namespace', '{']
    for i, (name, airports, w, h, data) in enumerate(rows_c):
        c.append('const uint8_t kBits%d[] = {%s};   // %s' % (i, ','.join('0x%02x' % v for v in data), name))
    c.append('const Landmarks::Landmark kAll[] = {')
    for i, (name, airports, w, h, data) in enumerate(rows_c):
        c.append('    {"%s", %d, %d, kBits%d},' % (name.replace('"', ''), w, h, i))
    c.append('};')
    c.append('struct Map { const char *iata; uint8_t i; };')
    c.append('const Map kAirports[] = {')
    for i, (name, airports, *_ ) in enumerate(rows_c):
        for a in airports:
            c.append('    {"%s", %d},' % (a, i))
    c.append('};')
    c.append('}')
    c.append('''
const Landmarks::Landmark *Landmarks::forAirport(const char *iata)
{
    if (!iata) return nullptr;
    for (const Map &m : kAirports)
        if (!strcmp(m.iata, iata)) return &kAll[m.i];
    return nullptr;
}''')
    (ROOT / 'firmware' / 'display' / 'Landmarks.h').write_text(h_out, encoding='utf-8', newline='\n')
    (ROOT / 'firmware' / 'display' / 'Landmarks.cpp').write_text('\n'.join(c) + '\n', encoding='utf-8', newline='\n')
    print('wrote %d landmarks, %d bytes of bitmaps' % (len(rows_c), sum(len(r[4]) for r in rows_c)))


def sheet(items, path):
    cols = 8
    rows = (len(items) + cols - 1) // cols
    cell = 6
    img = Image.new('RGB', (cols * (W + 4) * cell, rows * (H + 12) * cell), (20, 26, 40))
    d = ImageDraw.Draw(img)
    for i, (name, airports, bits) in enumerate(items):
        ox, oy = (i % cols) * (W + 4) * cell, (i // cols) * (H + 12) * cell
        for y in range(H):
            for x in range(W):
                col = (12, 14, 22) if bits[y][x] else (70, 110, 170)
                d.rectangle((ox + x * cell, oy + y * cell, ox + x * cell + cell - 2, oy + y * cell + cell - 2), fill=col)
        d.text((ox + 4, oy + H * cell + 6), name + ' ' + ' '.join(airports), fill=(230, 230, 230))
    img.save(path)
    print('sheet', path)


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--sheet', help='write a contact sheet PNG here too')
    a = ap.parse_args()
    items = render_all()
    write(items)
    if a.sheet:
        sheet(items, a.sheet)
