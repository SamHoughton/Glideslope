#!/usr/bin/env python3
"""
Record the board's showcase (Animations -> Showcase on its web page) as an
animated GIF for the README, rendered as a lit LED panel.

  python tools/record_demo.py 192.168.0.244            -> docs/showcase.gif
  python tools/record_demo.py glideslope.local --look golden

The showcase is scripted around the fictional flight GS101 (which carries the
Glideslope badge, so no airline's logo is recorded). While it runs the board
stops fetching (no web pauses, no real flights cutting in) and hides the home
marker. Each frame comes back run-length encoded (?z=1, usually 1-4 KB, ~25 ms)
with X-Show-Ms, the showcase time it was drawn at, so one run is placed
exactly on the timeline: no interleaving, no guessed timestamps.

Needs only Pillow.
"""
import argparse
import http.client
import pickle
import time
import urllib.request
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parent.parent
W, H = 128, 64

# Showcase timeline (s), matching NeoMatrixDisplay::stepShowcase.
END = 39.5 + 3.4         # the end of the take-off scene


def decode(raw):
    """[count][lo][hi] runs -> 8192 RGB565 pixels."""
    px = []
    for i in range(0, len(raw) - 2, 3):
        px.extend([raw[i + 1] | (raw[i + 2] << 8)] * raw[i])
    return tuple(px[:W * H]) if len(px) >= W * H else None


def grab(base):
    """(showcase ms, frame), or (None, frame) outside the showcase, or (None, None)."""
    host = base.split('//', 1)[1]
    try:
        c = http.client.HTTPConnection(host, timeout=4)
        c.request('GET', '/api/frame?z=1')
        r = c.getresponse()
        raw = r.read()
        ms = r.getheader('X-Show-Ms')
        c.close()
        px = decode(raw)
        ms = int(ms) if ms is not None and int(ms) < 4_000_000_000 else None
        return ms, px
    except (OSError, ValueError, http.client.HTTPException):
        return None, None


def start(base, look):
    url = base + '/api/demo/showcase' + (f'?look={look}' if look else '')
    for _ in range(3):
        try:
            urllib.request.urlopen(urllib.request.Request(url, data=b'', method='POST'), timeout=10).read()
            return
        except OSError:
            time.sleep(2)
    raise SystemExit('board not answering')


def record(base, look):
    start(base, look)
    samples = {}   # showcase ms -> frame
    t0 = time.time()
    while time.time() - t0 < END + 10:
        ms, px = grab(base)
        if px is None: continue
        if ms is None:
            if samples: break      # the showcase is over
            continue               # not started yet
        samples[ms] = px
    print(f'{len(samples)} frames over {max(samples) / 1000:.1f} s')
    return sorted(samples.items())


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


def at(samples, t):
    """The frame on the panel at showcase time t (s): the latest drawn by then."""
    ms = t * 1000
    best = samples[0][1]
    for m, px in samples:
        if m > ms: break
        best = px
    return best


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('host', help='board address, e.g. glideslope.local or 192.168.0.244')
    ap.add_argument('--look', default='golden', help='sky for the landing and take-off ("" for the real one)')
    ap.add_argument('--out', default=str(ROOT / 'docs' / 'showcase.gif'))
    ap.add_argument('--fps', type=float, default=20)   # 50 ms: GIF times are in 10 ms steps
    ap.add_argument('--reuse', action='store_true', help='re-render the last recording')
    a = ap.parse_args()
    base = 'http://' + a.host
    cache = ROOT / 'tools' / '.cache' / 'showcase_frames.pickle'
    if a.reuse and cache.exists():
        samples = pickle.loads(cache.read_bytes())
    else:
        samples = record(base, a.look)
        cache.parent.mkdir(exist_ok=True)
        cache.write_bytes(pickle.dumps(samples))

    end = min(END, samples[-1][0] / 1000)
    frames, durs = [], []
    step = 1.0 / a.fps
    t = 0.3
    while t < end:
        frames.append(at(samples, t))
        durs.append(int(step * 1000))
        t += step
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
    pal = mosaic.quantize(colors=192, method=Image.Quantize.MEDIANCUT)
    images = [im.quantize(palette=pal, dither=Image.Dither.NONE) for im in rendered]
    images[0].save(a.out, save_all=True, append_images=images[1:], duration=mdur, loop=0, optimize=True, disposal=1)
    print(f'{a.out}: {len(images)} frames, {Path(a.out).stat().st_size // 1024} KB')

    # A still of the approach card for social previews and the like.
    led_render(to_image(at(samples, 6.0)), cell=6, pad=20).save(Path(a.out).with_name('showcase-card.png'))


if __name__ == '__main__':
    main()
