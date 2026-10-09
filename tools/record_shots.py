#!/usr/bin/env python3
"""
The README's panel images, all from the board, in one go (run it for every
release, after tools/record_demo.py):

  python tools/record_shots.py 192.168.0.244

From the showcase recording (tools/.cache/showcase_frames.pickle, made by
record_demo.py): the fly-across, landing and take-off clips, and stills of
the arrivals board, weather, stats and holding screens. Captured live: the
emergency-squawk alert and the scanning screen (both demos, no real flight).
The sprite sheet comes from tools/sprite_gen.py.

Needs only Pillow.
"""
import argparse
import pickle
import sys
import time
import urllib.request
from pathlib import Path

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import record_demo as rd   # noqa: E402
import sprite_gen as sg    # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
DOCS = ROOT / 'docs'

# Showcase times (s), matching NeoMatrixDisplay::stepShowcase.
CLIPS = {'flyacross.gif': (0.0, 2.4), 'landing.gif': (9.0, 12.6), 'takeoff.gif': (39.5, 42.9)}
STILLS = {'arrivals.png': 24.5, 'weather.png': 29.5, 'stats.png': 34.3, 'holding.png': 38.5}


def save_gif(frames, path, fps=20):
    rendered = [rd.led_render(rd.to_image(px), cell=4, pad=10) for px in frames]
    pal = rendered[len(rendered) // 2].quantize(colors=160, method=Image.Quantize.MEDIANCUT)
    images = [im.quantize(palette=pal, dither=Image.Dither.NONE) for im in rendered]
    durs = [int(1000 / fps)] * len(images)
    durs[-1] = 1200
    images[0].save(path, save_all=True, append_images=images[1:], duration=durs, loop=0, optimize=True, disposal=1)
    print('wrote', path.name, len(images), 'frames')


def clip(samples, t0, t1, fps=20):
    out, t = [], t0
    while t < t1:
        out.append(rd.at(samples, t))
        t += 1.0 / fps
    return out


def post(base, path):
    for _ in range(3):
        try:
            urllib.request.urlopen(urllib.request.Request(base + path, data=b'', method='POST'), timeout=15).read()
            return
        except OSError:
            time.sleep(2)


def live(base, seconds):
    """Frames from the panel for a few seconds (outside the showcase)."""
    frames, t0 = [], time.time()
    while time.time() - t0 < seconds:
        _, px = rd.grab(base)
        if px: frames.append(px)
    return frames


def sprites_png(path):
    """Every type, each in a different airline's colours, as on the panel."""
    from PIL import ImageDraw
    airlines = ['BAW', 'VIR', 'EZY', 'RYR', 'KLM', 'UAE', 'DLH', 'AFR', 'QTR', 'AAL', 'DAL', 'ACA',
                'WZZ', 'SIA', 'IBE', 'TOM', 'EIN', 'FIN']
    S, cw, ch = 6, 48, 19
    im = Image.new('RGB', (3 * cw * S, 6 * ch * S), (5, 6, 8))
    dr = ImageDraw.Draw(im)
    for k, t in enumerate(sg.KINDS):
        cx, cy = (k % 3) * cw, (k // 3) * ch
        sg.background(dr, cx, cy, cw - 1, ch - 1, S)
        rows = sg.fit(t, sg.CARD_MAX, 1.0, False)
        sg.draw(dr, rows, cx + (cw - len(rows[0])) // 2, cy + (ch - len(rows)) // 2, S, sg.LIV[airlines[k]])
    im.save(path)
    print('wrote', path.name)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('host')
    a = ap.parse_args()
    base = 'http://' + a.host
    samples = pickle.loads((ROOT / 'tools' / '.cache' / 'showcase_frames.pickle').read_bytes())

    for name, (t0, t1) in CLIPS.items():
        save_gif(clip(samples, t0, t1), DOCS / name)
    for name, t in STILLS.items():
        rd.led_render(rd.to_image(rd.at(samples, t)), cell=6, pad=12).save(DOCS / name)
        print('wrote', name)

    post(base, '/api/demo/squawk')
    time.sleep(1.2)
    rd.led_render(rd.to_image(live(base, 0.3)[-1]), cell=6, pad=12).save(DOCS / 'squawk.png')
    print('wrote squawk.png')
    time.sleep(12)   # let the alert finish
    post(base, '/api/demo/splash')
    time.sleep(0.8)
    frames = live(base, 3.0)
    save_gif(frames[::max(1, len(frames) // 40)], DOCS / 'scanning.gif', fps=13)
    sprites_png(DOCS / 'sprites.png')


if __name__ == '__main__':
    main()
