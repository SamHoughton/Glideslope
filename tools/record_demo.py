#!/usr/bin/env python3
"""
Record the board's showcase (Animations -> Showcase on its web page) as an
animated GIF for the README, rendered as a lit LED panel.

  python tools/record_demo.py 192.168.0.244            -> docs/showcase.gif
  python tools/record_demo.py glideslope.local --runs 3

The showcase is scripted (the fictional flight GS101, which carries the
Glideslope badge, so no airline's logo is ever recorded), so several runs can
be interleaved: grabbing a frame over Wi-Fi takes ~80 ms, and three runs give
smooth animation. The map / arrivals / weather part shows live data, so it is
taken from the first run only.

Needs only Pillow.
"""
import argparse
import pickle
import struct
import time
import urllib.request
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parent.parent
W, H = 128, 64

# Showcase timeline (s), matching NeoMatrixDisplay::stepShowcase.
SCREENS_FROM, SCREENS_TO, END = 16.5, 29.5, 32.8   # END: the take-off's last frame


def grab(base):
    """One frame, or None: the board's web server pauses while it fetches
    flight data, so a request can wait or time out; other runs fill the gap."""
    try:
        raw = urllib.request.urlopen(base + '/api/frame', timeout=4).read()
        return struct.unpack('<8192H', raw) if len(raw) == 16384 else None
    except OSError:
        return None


def start(base):
    urllib.request.urlopen(urllib.request.Request(base + '/api/demo/showcase', data=b'', method='POST'),
                           timeout=5).read()


def record(base, runs):
    samples = []   # (seconds since start, run, frame)
    for run in range(runs):
        start(base)
        t0 = time.time()
        time.sleep(run * 0.027)   # stagger the runs so their samples interleave
        while time.time() - t0 < END:
            ts = time.time() - t0
            px = grab(base)
            if px and time.time() - t0 - ts < 0.4:          # skip answers delayed by a fetch
                samples.append((ts + 0.04, run, px))         # stamp mid-request
        print(f'run {run + 1}: {sum(1 for s in samples if s[1] == run)} frames')
        time.sleep(4)
    return samples


def to_image(px):
    im = Image.new('RGB', (W, H))
    im.putdata([(((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31) for v in px])
    return im


def led_render(im, cell=4, pad=14):
    """128x64 frame -> round LEDs on a dark panel, with a soft glow."""
    big = im.resize((W * cell, H * cell), Image.NEAREST)
    # Dot mask: a round LED in each cell.
    tile = Image.new('L', (cell, cell), 0)
    ImageDraw.Draw(tile).ellipse([0, 0, cell - 1, cell - 1], fill=255)
    mask = Image.new('L', big.size)
    for y in range(0, big.height, cell):
        for x in range(0, big.width, cell):
            mask.paste(tile, (x, y))
    lit = ImageChops.multiply(big, Image.merge('RGB', (mask, mask, mask)))
    lit = Image.eval(lit, lambda v: min(255, int(v * 1.2)))   # LEDs are bright
    glow = lit.filter(ImageFilter.GaussianBlur(cell * 0.9))
    glow = Image.eval(glow, lambda v: int(v * 0.6))
    panel = ImageChops.add(Image.new('RGB', big.size, (9, 10, 13)), ImageChops.screen(lit, glow))
    # Faint unlit LEDs, as on the real panel.
    unlit = Image.new('RGB', big.size, (17, 19, 24))
    dark = Image.eval(big.convert('L'), lambda v: 255 if v < 8 else 0)
    panel.paste(unlit, (0, 0), ImageChops.multiply(dark, Image.eval(mask, lambda v: v // 3)))
    out = Image.new('RGB', (big.width + 2 * pad, big.height + 2 * pad), (13, 17, 23))
    ImageDraw.Draw(out).rounded_rectangle([pad // 2, pad // 2, out.width - pad // 2 - 1, out.height - pad // 2 - 1],
                                          radius=10, fill=(5, 6, 8), outline=(40, 44, 52))
    out.paste(panel, (pad, pad))
    return out


def nearest(samples, t, runs=None):
    pool = [s for s in samples if runs is None or s[1] in runs]
    return min(pool, key=lambda s: abs(s[0] - t))[2]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('host', help='board address, e.g. glideslope.local or 192.168.0.244')
    ap.add_argument('--runs', type=int, default=3)
    ap.add_argument('--out', default=str(ROOT / 'docs' / 'showcase.gif'))
    ap.add_argument('--fps', type=float, default=15)
    ap.add_argument('--reuse', action='store_true', help='re-render the last recording')
    a = ap.parse_args()
    base = 'http://' + a.host
    cache = ROOT / 'tools' / '.cache' / 'showcase_samples.pickle'
    if a.reuse and cache.exists():
        samples = pickle.loads(cache.read_bytes())
    else:
        samples = record(base, a.runs)
        cache.parent.mkdir(exist_ok=True)
        cache.write_bytes(pickle.dumps(samples))

    frames, durs = [], []
    step = 1.0 / a.fps
    t = 0.0
    while t < END:
        live = SCREENS_FROM <= t < SCREENS_TO
        px = nearest(samples, t, {0} if live else None)
        frames.append(px)
        dt = 0.2 if live else step   # the screens tour barely moves: 5 fps is plenty
        durs.append(int(dt * 1000))
        t += dt
    durs[-1] = 1500

    # Merge identical neighbours, then render.
    merged, mdur = [], []
    for px, d in zip(frames, durs):
        if merged and merged[-1] == px:
            mdur[-1] += d
        else:
            merged.append(px)
            mdur.append(d)
    rendered = [led_render(to_image(px)) for px in merged]
    # One palette for the whole GIF, from a mosaic of frames across it: true
    # colours, and no shimmer from per-frame palettes.
    picks = rendered[::max(1, len(rendered) // 16)]
    mosaic = Image.new('RGB', (picks[0].width, picks[0].height * len(picks)))
    for i, im in enumerate(picks):
        mosaic.paste(im, (0, i * im.height))
    pal = mosaic.quantize(colors=128, method=Image.Quantize.MEDIANCUT)
    images = [im.quantize(palette=pal, dither=Image.Dither.NONE) for im in rendered]
    images[0].save(a.out, save_all=True, append_images=images[1:], duration=mdur, loop=0, optimize=True, disposal=1)
    print(f'{a.out}: {len(images)} frames, {Path(a.out).stat().st_size // 1024} KB')

    # A still of the approach card for social previews and the like.
    led_render(to_image(nearest(samples, 6.0)), cell=6, pad=20).save(Path(a.out).with_name('showcase-card.png'))


if __name__ == '__main__':
    main()
