#!/usr/bin/env python3
"""Generates g2c.ico from the two SVG templates.

Why two templates and not one?

An .ico contains a SEPARATE image for each size, and Windows picks the
matching one. That is not a workaround but the purpose of the format: what is
fine and legible at 256 pixels becomes a blob at 16.

  g2c-large.svg  -> 256, 48   with joint chain, thinner strokes
  g2c-small.svg  -> 32, 16    without chain, bolder strokes, larger glyphs

Usage:   python3 make_icon.py
Result:  g2c.ico next to the script.

Requires cairosvg and Pillow:
    pip install cairosvg pillow
"""

from pathlib import Path

import cairosvg
from PIL import Image

HERE = Path(__file__).resolve().parent

# Size -> template. The cutoff is 48: below that, the chain no longer adds
# anything and only takes up space.
PLAN = [
    (256, "g2c-large.svg"),
    (128, "g2c-large.svg"),
    (48, "g2c-large.svg"),
    (32, "g2c-small.svg"),
    (16, "g2c-small.svg"),
]


def render(svg: Path, size: int) -> Image.Image:
    png = cairosvg.svg2png(
        url=str(svg),
        output_width=size,
        output_height=size,
        background_color=None,
    )
    import io

    return Image.open(io.BytesIO(png)).convert("RGBA")


def main() -> int:
    frames = []
    for size, name in PLAN:
        svg = HERE / name
        if not svg.exists():
            print(f"FEHLER: {name} fehlt")
            return 1
        frames.append(render(svg, size))
        print(f"  {size:>3} px aus {name}")

    out = HERE / "g2c.ico"

    # With the first image, Pillow writes all sizes from sizes= - but it
    # scales them itself. That is why the images are passed INDIVIDUALLY:
    # this way each rendered version ends up in the file unchanged.
    frames[0].save(
        out,
        format="ICO",
        sizes=[(f.width, f.height) for f in frames],
        append_images=frames[1:],
    )
    print(f"\nGeschrieben: {out}  ({out.stat().st_size} Bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
