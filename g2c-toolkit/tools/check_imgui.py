#!/usr/bin/env python3
"""Prueft die ImGui-Aufrufe auf die Paarungsregeln.

ImGui verlangt bei einigen Funktionen, dass das Gegenstueck IMMER aufgerufen
wird — auch wenn Begin false liefert. Andere duerfen NUR bei true beendet
werden. Wer das verwechselt, bekommt eine Zusicherung zur Laufzeit, und zwar
oft erst dann, wenn ein Fenster minimiert oder ein Reiter zugeklappt wird.

Aus der offiziellen Beschreibung:

    Always call a matching End() for each Begin() call, regardless of its
    return value! ... this is inconsistent with most other BeginXXX
    functions.

Diese Datei kann ImGui nicht ausfuehren. Geprueft wird deshalb, was sich am
Quelltext ablesen laesst: Zaehlen die Paare? Steht ein PopID zu jedem PushID?
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Paare. Links stehen ALLE Varianten, die dasselbe Gegenstueck brauchen.
#
# Die Regel ist nicht "gleich oft": ein frueher Ausstieg
#
#     if (!ImGui::Begin(...)) { ImGui::End(); return; }
#     ...
#     ImGui::End();
#
# ist korrekt und ergibt MEHR End als Begin. Zu wenige End sind der Fehler,
# zu viele sind ein Hinweis auf mehrere Ausgaenge.
PAIRE = [
    (["ImGui::Begin("], "ImGui::End("),
    (["ImGui::BeginChild("], "ImGui::EndChild("),
    (["ImGui::PushID("], "ImGui::PopID("),
    (["ImGui::BeginMenu("], "ImGui::EndMenu("),
    (["ImGui::BeginMenuBar("], "ImGui::EndMenuBar("),
    (["ImGui::BeginTable("], "ImGui::EndTable("),
    (["ImGui::BeginCombo("], "ImGui::EndCombo("),
    (["ImGui::BeginPopup(", "ImGui::BeginPopupModal(",
      "ImGui::BeginPopupContextItem(", "ImGui::BeginPopupContextWindow("],
     "ImGui::EndPopup("),
    (["ImGui::BeginTabBar("], "ImGui::EndTabBar("),
    (["ImGui::BeginTabItem("], "ImGui::EndTabItem("),
    (["ImGui::BeginDisabled("], "ImGui::EndDisabled("),
]


def zaehle(text: str, was: str) -> int:
    """Zaehlt Vorkommen ausserhalb von Kommentaren."""
    n = 0
    for zeile in text.split("\n"):
        code = zeile.split("//")[0]
        n += code.count(was)
    return n


def main() -> int:
    fehler = 0
    for datei in sorted((ROOT / "gui").glob("*.cpp")):
        text = datei.read_text(encoding="utf-8", errors="replace")
        for aufListe, zu in PAIRE:
            a = sum(zaehle(text, x) for x in aufListe)
            z = zaehle(text, zu)
            # Zu WENIG Gegenstuecke ist der Fehler. Mehr heisst nur, dass es
            # Funktionen mit mehreren Ausgaengen gibt.
            if z < a:
                name = aufListe[0].rstrip("(")
                print(f"  FEHLER: {datei.name}: {a}x {name}, aber nur {z}x "
                      f"{zu.rstrip('(')} — fehlendes Gegenstueck")
                fehler += 1

        # PushStyleColor nimmt eine Zahl mit; PopStyleColor(n) gibt n frei.
        push = sum(len(re.findall(r"PushStyleColor\(", z.split("//")[0]))
                   for z in text.split("\n"))
        pop = 0
        for z in text.split("\n"):
            for m in re.finditer(r"PopStyleColor\((\d*)\)", z.split("//")[0]):
                pop += int(m.group(1)) if m.group(1) else 1
        if push != pop:
            print(f"  FEHLER: {datei.name}: {push}x PushStyleColor, aber {pop} freigegeben")
            fehler += 1

        push = zaehle(text, "ImGui::PushStyleVar(")
        pop = 0
        for z in text.split("\n"):
            for m in re.finditer(r"PopStyleVar\((\d*)\)", z.split("//")[0]):
                pop += int(m.group(1)) if m.group(1) else 1
        if push != pop:
            print(f"  FEHLER: {datei.name}: {push}x PushStyleVar, aber {pop} freigegeben")
            fehler += 1

    # Die Spaltenzahl in BeginTable muss zur Anzahl der
    # TableSetupColumn-Aufrufe passen.
    #
    # Stimmt sie nicht, meldet ImGui zur LAUFZEIT
    # "Called TableSetupColumn() too many times" — und die ueberzaehlige
    # Spalte rutscht sichtbar in eine eigene Zeile. Beim Uebersetzen faellt
    # davon nichts auf.
    for datei in sorted((ROOT / "gui").glob("*.cpp")):
        text = datei.read_text(encoding="utf-8", errors="replace")
        for m in re.finditer(r'BeginTable\(\s*"([^"]+)"\s*,\s*(\d+)', text):
            name, deklariert = m.group(1), int(m.group(2))

            # Bis zum zugehoerigen EndTable zaehlen.
            rest = text[m.end():]
            ende = rest.find("EndTable()")
            bereich = rest[:ende] if ende >= 0 else rest

            # Nur Aufrufe VOR der ersten Datenzeile zaehlen: danach kommen
            # keine Spaltendefinitionen mehr.
            erste_zeile = bereich.find("TableHeadersRow()")
            kopf = bereich[:erste_zeile] if erste_zeile >= 0 else bereich

            n = zaehle(kopf, "TableSetupColumn(")
            if n and n != deklariert:
                print(f"  FEHLER: {datei.name}: BeginTable {name} mit {deklariert} "
                      f"Spalten, aber {n}x TableSetupColumn")
                fehler += 1

    if fehler:
        print(f"\n  {fehler} Beanstandung(en) in den ImGui-Aufrufen")
        return 1
    print("  ImGui-Aufrufe: Paare stimmen")
    return 0


if __name__ == "__main__":
    sys.exit(main())
