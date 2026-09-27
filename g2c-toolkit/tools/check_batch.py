#!/usr/bin/env python3
"""Static check of build.bat.

Batch can't be executed here. So what gets checked is whatever can be
checked without running it:

  * every "goto" has a label
  * every label is actually jumped to (otherwise it is dead code)
  * parentheses are balanced
  * no bare "exit" - that ends the whole command prompt, not just the
    script, and slams the window shut in the user's face
"""

import re
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parent.parent / "build.bat"


def main() -> int:
    raw = SRC.read_bytes()
    errors = 0

    # Line endings. This is not a cosmetic issue.
    #
    # cmd.exe behaves unreliably with Unix line endings: labels don't work,
    # commands run together, and a "pause" at the end gets skipped - the
    # window closes before anyone can read anything. That is exactly what
    # happened here after an edit had replaced the CRLFs.
    crlf = raw.count(b"\r\n")
    lf = raw.count(b"\n")
    if lf and crlf != lf:
        print(f"  FEHLER: {lf - crlf} Zeile(n) ohne CRLF. cmd.exe braucht "
              f"Windows-Zeilenenden.")
        errors += 1

    # Non-ASCII. The code page of a command prompt is not predictable (850 in
    # Germany, 857 in Turkey, 437 in the US); umlauts and dashes show up there
    # as garbage.
    nonascii = [i for i, b in enumerate(raw) if b > 127]
    if nonascii:
        line = raw[: nonascii[0]].count(b"\n") + 1
        print(f"  FEHLER: {len(nonascii)} Byte(s) ueber 127, zuerst in Zeile {line}. "
              f"Nur ASCII verwenden.")
        errors += 1

    # The wrapper must be present.
    #
    # Without it, a failed "goto" ends the file immediately and silently -
    # the window closes, and the message explaining why disappears with it.
    #
    # Check line by line, not in the whole text: "call :main" also appears in
    # the comment above it, and a comment calls nothing. The first version
    # of this check fell for exactly that.
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
        # And the wrapper must actually keep the window open.
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
    # "call :label" is a jump as well - and the most important one here: the
    # wrapper calls :main this way, so that an abort inside it returns
    # instead of closing the window.
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

        # Remove parentheses that are not blocks:
        #   %ProgramFiles(x86)%   environment variable with a parenthesis in its name
        #   [17.0^,18.0^)         parenthesis escaped with ^ in an argument
        #   "..."                 parentheses inside strings
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

    # Further Windows files with the same requirements.
    #
    # .rc goes through rc.exe, .bat through cmd.exe - both expect Windows
    # line endings and can't handle bytes above 127, because the code page
    # is not predictable.
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

    # Also check start_build.bat, if present.
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
