#!/usr/bin/env python3
"""Checks the ImGui calls against the pairing rules.

For some functions, ImGui requires the counterpart to ALWAYS be called -
even if Begin returns false. Others may ONLY be ended on true. Mixing these
up gets you an assertion at runtime, and often only once a window is
minimized or a tab is collapsed.

From the official documentation:

    Always call a matching End() for each Begin() call, regardless of its
    return value! ... this is inconsistent with most other BeginXXX
    functions.

This file can't run ImGui. So it checks what can be read from the source:
do the pairs add up? Is there a PopID for every PushID?
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Pairs. On the left are ALL variants that need the same counterpart.
#
# The rule is not "equally often": an early exit
#
#     if (!ImGui::Begin(...)) { ImGui::End(); return; }
#     ...
#     ImGui::End();
#
# is correct and yields MORE End than Begin. Too few End calls are the bug,
# too many just indicate multiple exits.
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
    """Counts occurrences outside of comments."""
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
            # Too FEW counterparts is the bug. More just means there are
            # functions with multiple exits.
            if z < a:
                name = aufListe[0].rstrip("(")
                print(f"  FEHLER: {datei.name}: {a}x {name}, aber nur {z}x "
                      f"{zu.rstrip('(')} — fehlendes Gegenstueck")
                fehler += 1

        # PushStyleColor pushes one entry; PopStyleColor(n) pops n.
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

    # The column count in BeginTable must match the number of
    # TableSetupColumn calls.
    #
    # If it doesn't, ImGui reports at RUNTIME
    # "Called TableSetupColumn() too many times" - and the extra column
    # visibly slips into a row of its own. Nothing of this shows up at
    # compile time.
    for datei in sorted((ROOT / "gui").glob("*.cpp")):
        text = datei.read_text(encoding="utf-8", errors="replace")
        for m in re.finditer(r'BeginTable\(\s*"([^"]+)"\s*,\s*(\d+)', text):
            name, deklariert = m.group(1), int(m.group(2))

            # Count up to the matching EndTable.
            rest = text[m.end():]
            ende = rest.find("EndTable()")
            bereich = rest[:ende] if ende >= 0 else rest

            # Only count calls BEFORE the first data row: after that, no more
            # column definitions follow.
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
