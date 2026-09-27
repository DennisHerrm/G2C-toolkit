#!/usr/bin/env python3
"""Finds state fields that are set but never read.

Why this is necessary:

Several times in this project, a replacement missed its target and silently
removed a piece of code. When reordering sequences, it hit the execution
step: the context menu set `pendingBlock_`, but nobody read the field
anymore. The compiler says nothing about that - assigning to an existing
field is perfectly legal. The tests said nothing either, because the data
operation itself was fine; only the wiring was missing.

From the user's point of view that meant: click, and nothing happens.

A field that is only ever written to is almost always exactly such a case.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FILES = [ROOT / "gui" / "app.cpp", ROOT / "gui" / "app.h"]

# Fields that are intentionally only written.
ALLOWED = {
    "frameWorkerBaseDir_",   # set only for traceability
}


def plattformrueckrufe(gui: Path) -> int:
    """Every Platform callback must be set AND used.

    A callback that main_win32.cpp sets but the UI never calls is dead
    code - and conversely, a call that goes nowhere.

    That is exactly what happened with pickFolders: the dialog was there, but
    the buttons kept calling the old one. The bug only showed in that you
    couldn't select a second folder in the dialog.
    """
    fehler = 0
    header = (gui / "app.h").read_text(encoding="utf-8", errors="replace")
    app = (gui / "app.cpp").read_text(encoding="utf-8", errors="replace")
    win = (gui / "main_win32.cpp").read_text(encoding="utf-8", errors="replace")

    # Extract the callbacks from the Platform struct.
    i = header.find("struct Platform")
    if i < 0:
        return 0
    block = header[i:header.find("};", i)]
    namen = re.findall(r">\s*(\w+);", block)

    for n in namen:
        gesetzt = f"plat.{n}" in win
        benutzt = f"platform_.{n}" in app
        if gesetzt and not benutzt:
            print(f"  FEHLER: Platform::{n} wird gesetzt, aber nie benutzt")
            fehler += 1
        elif benutzt and not gesetzt:
            print(f"  FEHLER: Platform::{n} wird benutzt, aber nie gesetzt")
            fehler += 1
    return fehler


def main() -> int:
    header = (ROOT / "gui" / "app.h").read_text(encoding="utf-8")
    source = (ROOT / "gui" / "app.cpp").read_text(encoding="utf-8")
    both = header + "\n" + source

    # Class fields: they end in an underscore.
    members = set(re.findall(r"\b(\w+_)\s*[;=({]", header))
    members = {m for m in members if not m.startswith("_")} - ALLOWED

    errors = 0
    for m in sorted(members):
        writes = 0
        reads = 0
        for occ in re.finditer(r"(?<![\w.>])" + re.escape(m) + r"\b", both):
            after = both[occ.end():occ.end() + 40]
            before = both[max(0, occ.start() - 30):occ.start()]

            # Skip the declaration in the header.
            if re.match(r"\s*[;={]", after) and "return" not in before and "(" not in before:
                if occ.start() < len(header):
                    continue

            # Assignment: field followed by = (but not == != <= >=).
            if re.match(r"\s*=[^=]", after):
                writes += 1
            elif re.match(r"\s*\.(clear|assign|push_back|erase|insert|reset)\b", after):
                writes += 1
            else:
                reads += 1

        if writes > 0 and reads == 0:
            print(f"  FEHLER: '{m}' wird {writes}x gesetzt, aber nie gelesen "
                  f"— fehlt die Auswertung?")
            errors += 1

    errors += plattformrueckrufe(ROOT / "gui")

    if errors:
        print(f"\n  {errors} Beanstandung(en) in der Oberflaechenverdrahtung")
        return 1
    print("  Oberflaechenverdrahtung: keine ungelesenen Felder")
    return 0


if __name__ == "__main__":
    sys.exit(main())
