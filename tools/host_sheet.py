#!/usr/bin/env python3
"""
Contact sheet of frames from tools/host_render.cpp, drawn as LEDs.

    python tools/host_sheet.py FRAMES_DIR OUT.png [name-prefix ...]
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, str(Path(__file__).parent))
from record_demo import led_render  # noqa: E402


def main():
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    prefixes = sys.argv[3:]
    frames = sorted(p for p in src.glob("*.ppm") if not prefixes or any(p.stem.startswith(x) for x in prefixes))
    tiles = [(p.stem, led_render(Image.open(p).convert("RGB"), 4, 14)) for p in frames]
    if not tiles:
        sys.exit("no frames")
    tw, th = tiles[0][1].size
    cols = 2 if len(tiles) > 1 else 1
    rows = (len(tiles) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * (tw + 20) + 20, rows * (th + 46) + 20), (13, 17, 23))
    d = ImageDraw.Draw(sheet)
    try:
        font = ImageFont.truetype("segoeui.ttf", 20)
    except OSError:
        font = None
    for i, (name, im) in enumerate(tiles):
        x, y = 20 + (i % cols) * (tw + 20), 20 + (i // cols) * (th + 46)
        d.text((x + 4, y), name, fill=(200, 210, 225), font=font)
        sheet.paste(im, (x, y + 30))
    sheet.save(out)
    print(out, sheet.size)


if __name__ == "__main__":
    main()
