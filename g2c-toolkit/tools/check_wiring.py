#!/usr/bin/env python3
"""Findet Zustandsfelder, die gesetzt, aber nie gelesen werden.

Warum das noetig ist:

Mehrfach ist in diesem Projekt eine Ersetzung ins Leere gelaufen und hat
still eine Codestelle entfernt. Beim Umordnen der Sequenzen traf es die
Ausfuehrung: das Kontextmenue setzte `pendingBlock_`, aber niemand las das
Feld mehr aus. Der Compiler schweigt dazu — eine Zuweisung an ein
existierendes Feld ist voellig legal. Die Tests schwiegen auch, weil die
Datenoperation selbst in Ordnung war; nur die Verdrahtung fehlte.

Aus Sicht des Nutzers hiess das: Klick, und nichts passiert.

Ein Feld, in das nur geschrieben wird, ist fast immer genau so ein Fall.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FILES = [ROOT / "gui" / "app.cpp", ROOT / "gui" / "app.h"]

# Felder, die absichtlich nur geschrieben werden.
ALLOWED = {
    "frameWorkerBaseDir_",   # nur zur Nachvollziehbarkeit gesetzt
}


def plattformrueckrufe(gui: Path) -> int:
    """Jeder Platform-Rueckruf muss gesetzt UND benutzt werden.

    Ein Rueckruf, den main_win32.cpp setzt, den die Oberflaeche aber nie
    aufruft, ist toter Code — und umgekehrt ein Aufruf ins Leere.

    Genau das ist mit pickFolders passiert: der Dialog war da, die Knoepfe
    riefen weiter den alten. Der Fehler zeigte sich nur daran, dass sich im
    Dialog kein zweiter Ordner markieren liess.
    """
    fehler = 0
    header = (gui / "app.h").read_text(encoding="utf-8", errors="replace")
    app = (gui / "app.cpp").read_text(encoding="utf-8", errors="replace")
    win = (gui / "main_win32.cpp").read_text(encoding="utf-8", errors="replace")

    # Rueckrufe aus der Platform-Struktur herausziehen.
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

    # Felder der Klasse: enden auf einen Unterstrich.
    members = set(re.findall(r"\b(\w+_)\s*[;=({]", header))
    members = {m for m in members if not m.startswith("_")} - ALLOWED

    errors = 0
    for m in sorted(members):
        writes = 0
        reads = 0
        for occ in re.finditer(r"(?<![\w.>])" + re.escape(m) + r"\b", both):
            after = both[occ.end():occ.end() + 40]
            before = both[max(0, occ.start() - 30):occ.start()]

            # Deklaration im Header ueberspringen.
            if re.match(r"\s*[;={]", after) and "return" not in before and "(" not in before:
                if occ.start() < len(header):
                    continue

            # Zuweisung: Feld gefolgt von = (aber nicht == != <= >=).
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
