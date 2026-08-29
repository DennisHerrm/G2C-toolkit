#!/usr/bin/env python3
"""Erzeugt assets/g2c.ico aus derselben Geometrie wie assets/g2c.svg.

Warum nicht die SVG rastern? Das braeuchte cairosvg oder Inkscape — eine
Abhaengigkeit, die man erst installieren muss. Die Form ist einfach genug,
um sie direkt zu zeichnen; noetig ist nur Pillow.

Die .ico enthaelt vier Groessen, und die kleinen sind NICHT einfach
herunterskaliert:

  256, 48  volle Zeichnung mit Gelenkkette
  32, 16   nur die Zeichen, Kette weggelassen

Bei 16 Pixeln waere die Kette drei ununterscheidbare Punkte und wuerde die
Zeichen nur verschmieren. Genau dafuer erlaubt das Format ein eigenes Bild
je Groesse — Windows waehlt selbst die passende.

Gezeichnet wird immer in 8-facher Groesse und danach mit LANCZOS
verkleinert. Pillow kann keine Kantenglaettung beim Zeichnen; ohne diesen
Umweg haetten die Rundungen Treppen.
"""

from pathlib import Path

from PIL import Image, ImageDraw

OUT = Path(__file__).resolve().parent.parent / "assets" / "g2c.ico"

TILE = (0x0F, 0x6E, 0x56)
GLYPH = (0xFF, 0xFF, 0xFF)
CHAIN = (0x5D, 0xCA, 0xA5)

SS = 8          # Ueberabtastung
BASE = 96       # Koordinatensystem der SVG


def draw(size: int, with_chain: bool) -> Image.Image:
    n = size * SS
    k = n / BASE                      # Umrechnung SVG-Einheit -> Bildpunkt
    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    def S(v):
        return v * k

    d.rounded_rectangle([0, 0, n - 1, n - 1], radius=S(20), fill=TILE)

    w = S(11)                          # Strichstaerke der Zeichen

    # g: Bauch
    r = S(11)
    cx, cy = S(29), S(40)
    d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=GLYPH, width=int(round(w)))

    # g: Stamm mit Schwanz. Die Kurve wird als Streckenzug angenaehert —
    # bei achtfacher Aufloesung ist der Unterschied zur echten Bezierkurve
    # nach dem Verkleinern nicht mehr sichtbar.
    stem = [(S(41), S(29)), (S(41), S(53))]
    for i in range(1, 13):
        t = i / 12
        x = (1 - t) ** 2 * S(41) + 2 * (1 - t) * t * S(41) + t * t * S(30)
        y = (1 - t) ** 2 * S(53) + 2 * (1 - t) * t * S(65) + t * t * S(63)
        stem.append((x, y))
    d.line(stem, fill=GLYPH, width=int(round(w)), joint="curve")
    _cap(d, stem[0], w, GLYPH)
    _cap(d, stem[-1], w, GLYPH)

    # 2: obere Rundung, Schraege, Fuss
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
    """Runde Kappe. Pillows line() zeichnet stumpfe Enden."""
    x, y = pt
    r = w / 2
    d.ellipse([x - r, y - r, x + r, y + r], fill=color)


def main():
    OUT.parent.mkdir(parents=True, exist_ok=True)
    imgs = [draw(256, True), draw(48, True), draw(32, False), draw(16, False)]
    imgs[0].save(OUT, format="ICO", sizes=[(i.width, i.height) for i in imgs],
                 append_images=imgs[1:])
    print(f"  {OUT}  ({OUT.stat().st_size} Bytes, 4 Groessen)")

    # Zur Sichtkontrolle auch als PNG.
    for im in imgs:
        p = OUT.with_name(f"g2c_{im.width}.png")
        im.save(p)
        print(f"  {p}")


if __name__ == "__main__":
    main()
