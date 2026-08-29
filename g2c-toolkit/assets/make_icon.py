#!/usr/bin/env python3
"""Erzeugt g2c.ico aus den beiden SVG-Vorlagen.

Warum zwei Vorlagen und nicht eine?

Eine .ico enthaelt fuer jede Groesse ein EIGENES Bild, und Windows waehlt
die passende aus. Das ist kein Umweg, sondern der Zweck des Formats: was bei
256 Bildpunkten fein und lesbar ist, wird bei 16 zu einem Fleck.

  g2c-large.svg  -> 256, 48   mit Gelenkkette, duennere Striche
  g2c-small.svg  -> 32, 16    ohne Kette, kraeftigere Striche, groessere Zeichen

Aufruf:  python3 make_icon.py
Ergebnis: g2c.ico neben dem Skript.

Braucht cairosvg und Pillow:
    pip install cairosvg pillow
"""

from pathlib import Path

import cairosvg
from PIL import Image

HERE = Path(__file__).resolve().parent

# Groesse -> Vorlage. Die Grenze liegt bei 48: darunter traegt die Kette
# nichts mehr bei und kostet nur Platz.
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

    # Pillow schreibt beim ersten Bild alle Groessen aus sizes= — es
    # skaliert dabei aber selbst. Deshalb die Bilder EINZELN uebergeben:
    # so landet jede gerenderte Fassung unveraendert in der Datei.
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
