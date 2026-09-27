#!/usr/bin/env python3
"""Generates assets/g2c.ico from the same geometry as assets/g2c.svg.

Why not rasterize the SVG? That would need cairosvg or Inkscape - a
dependency you have to install first. The shape is simple enough to draw
directly; all that's needed is Pillow.

The .ico contains four sizes, and the small ones are NOT simply scaled
down:

  256, 48  full drawing with joint chain
  32, 16   only the glyphs, chain left out

At 16 pixels the chain would be three indistinguishable dots and would only
smear the glyphs. This is exactly why the format allows a separate image per
size - Windows picks the matching one itself.

Everything is always drawn at 8x size and then downscaled with LANCZOS.
Pillow can't anti-alias while drawing; without this detour the curves would
have jagged steps.
"""

from pathlib import Path

from PIL import Image, ImageDraw

OUT = Path(__file__).resolve().parent.parent / "assets" / "g2c.ico"

TILE = (0x0F, 0x6E, 0x56)
GLYPH = (0xFF, 0xFF, 0xFF)
CHAIN = (0x5D, 0xCA, 0xA5)

SS = 8          # supersampling
BASE = 96       # coordinate system of the SVG


def draw(size: int, with_chain: bool) -> Image.Image:
    n = size * SS
    k = n / BASE                      # conversion SVG unit -> pixel
    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    def S(v):
        return v * k

    d.rounded_rectangle([0, 0, n - 1, n - 1], radius=S(20), fill=TILE)

    w = S(11)                          # stroke width of the glyphs

    # g: bowl
    r = S(11)
    cx, cy = S(29), S(40)
    d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=GLYPH, width=int(round(w)))

    # g: stem with tail. The curve is approximated as a polyline - at eight
    # times the resolution, the difference from a real Bezier curve is no
    # longer visible after downscaling.
    stem = [(S(41), S(29)), (S(41), S(53))]
    for i in range(1, 13):
        t = i / 12
        x = (1 - t) ** 2 * S(41) + 2 * (1 - t) * t * S(41) + t * t * S(30)
        y = (1 - t) ** 2 * S(53) + 2 * (1 - t) * t * S(65) + t * t * S(63)
        stem.append((x, y))
    d.line(stem, fill=GLYPH, width=int(round(w)), joint="curve")
    _cap(d, stem[0], w, GLYPH)
    _cap(d, stem[-1], w, GLYPH)

    # 2: upper curve, diagonal, foot
    two = []
    for i in range(0, 13):
        t = i / 12
        x = (1 - t) ** 2 * S(55) + 2 * (1 - t) * t * S(55) + t * t * S(66)
        y = (1 - t) ** 2 * S(32) + 2 * (1 - t) * t * S(22) + t * t * S(22)
        two.append((x, y))
    for i in range(1, 13):
        t = i / 12
        x = (1 - t) ** 2 * S(66) + 2 * (1 - t) * t * S(77) + t * t * S(77)
        y = (1 - t) ** 2 * S(22) + 2 * (1 - t) * t * S(22) + t * t * S(31)
        two.append((x, y))
    for i in range(1, 13):
        t = i / 12
        x = (1 - t) ** 2 * S(77) + 2 * (1 - t) * t * S(77) + t * t * S(55)
        y = (1 - t) ** 2 * S(31) + 2 * (1 - t) * t * S(40) + t * t * S(59)
        two.append((x, y))
    two.append((S(80), S(59)))
    d.line(two, fill=GLYPH, width=int(round(w)), joint="curve")
    _cap(d, two[0], w, GLYPH)
    _cap(d, two[-1], w, GLYPH)

    if with_chain:
        cw = S(4)
        d.line([(S(24), S(84)), (S(48), S(77))], fill=CHAIN, width=int(round(cw)))
        d.line([(S(48), S(77)), (S(72), S(84))], fill=CHAIN, width=int(round(cw)))
        for px, py in ((S(24), S(84)), (S(48), S(77)), (S(72), S(84))):
            rr = S(5)
            d.ellipse([px - rr, py - rr, px + rr, py + rr], fill=CHAIN)

    return img.resize((size, size), Image.LANCZOS)


def _cap(d, pt, w, color):
    """Round cap. Pillow's line() draws flat ends."""
    x, y = pt
    r = w / 2
    d.ellipse([x - r, y - r, x + r, y + r], fill=color)


def main():
    OUT.parent.mkdir(parents=True, exist_ok=True)
    imgs = [draw(256, True), draw(48, True), draw(32, False), draw(16, False)]
    imgs[0].save(OUT, format="ICO", sizes=[(i.width, i.height) for i in imgs],
                 append_images=imgs[1:])
    print(f"  {OUT}  ({OUT.stat().st_size} Bytes, 4 Groessen)")

    # Also as PNG for visual inspection.
    for im in imgs:
        p = OUT.with_name(f"g2c_{im.width}.png")
        im.save(p)
        print(f"  {p}")


if __name__ == "__main__":
    main()
