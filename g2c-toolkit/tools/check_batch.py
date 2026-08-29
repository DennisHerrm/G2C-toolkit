#!/usr/bin/env python3
"""Statische Pruefung von build.bat.

Batch laesst sich hier nicht ausfuehren. Geprueft wird deshalb, was sich
ohne Ausfuehrung pruefen laesst:

  * jedes "goto" hat eine Sprungmarke
  * jede Sprungmarke wird auch angesprungen (sonst ist sie toter Code)
  * Klammern sind ausgeglichen
  * kein blankes "exit" - das beendet die ganze Eingabeaufforderung, nicht
    nur das Skript, und schliesst dem Nutzer das Fenster vor der Nase zu
"""

import re
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parent.parent / "build.bat"


def main() -> int:
    raw = SRC.read_bytes()
    errors = 0

    # Zeilenenden. Das ist kein Schoenheitsfehler.
    #
    # cmd.exe verhaelt sich mit Unix-Zeilenenden unzuverlaessig: Sprungmarken
    # greifen nicht, Befehle laufen zusammen, und ein "pause" am Ende wird
    # uebersprungen - das Fenster geht zu, bevor jemand etwas lesen kann.
    # Genau das ist hier passiert, nachdem eine Bearbeitung die CRLF
    # ersetzt hatte.
    crlf = raw.count(b"\r\n")
    lf = raw.count(b"\n")
    if lf and crlf != lf:
        print(f"  FEHLER: {lf - crlf} Zeile(n) ohne CRLF. cmd.exe braucht "
              f"Windows-Zeilenenden.")
        errors += 1

    # Nicht-ASCII. Die Codepage einer Eingabeaufforderung ist nicht
    # vorhersagbar (850 in Deutschland, 857 in der Tuerkei, 437 in den USA);
    # Umlaute und Gedankenstriche erscheinen dort als Unsinn.
    nonascii = [i for i, b in enumerate(raw) if b > 127]
    if nonascii:
        line = raw[: nonascii[0]].count(b"\n") + 1
        print(f"  FEHLER: {len(nonascii)} Byte(s) ueber 127, zuerst in Zeile {line}. "
              f"Nur ASCII verwenden.")
        errors += 1

    # Der Mantel muss vorhanden sein.
    #
    # Ohne ihn beendet ein fehlgeschlagenes "goto" die Datei sofort und
    # wortlos - das Fenster geht zu, und die Meldung mit dem Grund
    # verschwindet mit ihm.
    #
    # Zeilenweise pruefen, nicht im Gesamttext: "call :main" steht auch im
    # Kommentar darueber, und ein Kommentar ruft nichts auf. Der erste
    # Versuch dieser Pruefung ist genau darauf hereingefallen.
    def is_code(line: str) -> bool:
        t = line.strip().lower()
        return bool(t) and not t.startswith("rem") and not t.startswith("::")

    code_lines = [l for l in raw.decode("ascii", "replace").split("\n") if is_code(l)]
    calls_main = any(l.strip().lower().startswith("call :main") for l in code_lines)
    if not calls_main:
        print("  FEHLER: build.bat ruft :main nicht ueber 'call' auf. Ohne diesen "
              "Mantel schliesst sich das Fenster bei einem Fehler wortlos.")
        errors += 1
    else:
        # Und der Mantel muss auch wirklich offen halten.
        if not any(l.strip().lower() == "cmd /k" for l in code_lines):
            print("  FEHLER: kein 'cmd /k' im Mantel - das Fenster bliebe nicht offen")
            errors += 1

    if raw[:3] == b"\xef\xbb\xbf":
        print("  FEHLER: BOM am Dateianfang. cmd.exe stolpert darueber.")
        errors += 1

    text = raw.decode("ascii", errors="replace")
    lines = text.split("\n")

    labels = {m.group(1).lower() for m in re.finditer(r"^\s*:([A-Za-z_]\w*)", text, re.M)}
    gotos = {m.group(1).lower() for m in re.finditer(r"\bgoto\s+:?([A-Za-z_]\w*)", text, re.I)}
    # "call :marke" ist ebenfalls ein Sprung - und der wichtigste hier: der
    # Mantel ruft :main so auf, damit ein Abbruch darin zurueckkehrt statt
    # das Fenster zu schliessen.
    gotos |= {m.group(1).lower() for m in re.finditer(r"\bcall\s+:([A-Za-z_]\w*)", text, re.I)}
    gotos.discard("eof")

    for g in sorted(gotos - labels):
        print(f"  FEHLER: 'goto {g}' springt ins Leere")
        errors += 1
    for l in sorted(labels - gotos):
        print(f"  Hinweis: Sprungmarke ':{l}' wird nie angesprungen")

    depth = 0
    for i, line in enumerate(lines, 1):
        code = line.split("REM ")[0]
        if code.strip().lower().startswith("rem"):
            continue

        # Klammern, die keine Bloecke sind, herausnehmen:
        #   %ProgramFiles(x86)%   Umgebungsvariable mit Klammer im Namen
        #   [17.0^,18.0^)         mit ^ maskierte Klammer in einem Argument
        #   "..."                 Klammern in Zeichenketten
        code = re.sub(r"%[^%]*%", "", code)
        code = re.sub(r'"[^"]*"', "", code)
        code = re.sub(r"\^.", "", code)

        depth += code.count("(") - code.count(")")
        if depth < 0:
            print(f"  FEHLER (Zeile {i}): schliessende Klammer ohne oeffnende")
            errors += 1
            depth = 0
    if depth != 0:
        print(f"  FEHLER: {depth} Klammer(n) nicht geschlossen")
        errors += 1

    # Weitere Windows-Dateien mit denselben Anforderungen.
    #
    # .rc geht durch rc.exe, .bat durch cmd.exe - beide erwarten
    # Windows-Zeilenenden und vertragen keine Bytes ueber 127, weil die
    # Codepage nicht vorhersagbar ist.
    for extra in ("start_build.bat", "fenster_test.bat", "../assets/g2c.rc"):
        f = SRC.parent / extra
        if not f.exists():
            continue
        b = f.read_bytes()
        n_lf, n_crlf = b.count(b"\n"), b.count(b"\r\n")
        if n_lf and n_crlf != n_lf:
            print(f"  FEHLER: {f.name} hat {n_lf - n_crlf} Zeile(n) ohne CRLF")
            errors += 1
        if any(x > 127 for x in b):
            print(f"  FEHLER: {f.name} enthaelt Bytes ueber 127")
            errors += 1

    # start_build.bat mitpruefen, wenn vorhanden.
    wrapper = SRC.parent / "start_build.bat"
    if wrapper.exists():
        w = wrapper.read_text(encoding="utf-8", errors="replace")
        if "build.bat" not in w:
            print("  FEHLER: start_build.bat ruft build.bat nicht auf")
            errors += 1

    for i, line in enumerate(lines, 1):
        if re.match(r"^\s*exit\s*$", line, re.I):
            print(f"  FEHLER (Zeile {i}): blankes 'exit' beendet die ganze "
                  f"Eingabeaufforderung - 'exit /b' verwenden")
            errors += 1

    if errors:
        print(f"\n  {errors} Beanstandung(en) in build.bat")
        return 1
    print("  build.bat: keine Beanstandungen")
    return 0


if __name__ == "__main__":
    sys.exit(main())
