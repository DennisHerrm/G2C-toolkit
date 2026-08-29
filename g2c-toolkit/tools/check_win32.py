#!/usr/bin/env python3
"""Statische Pruefung von gui/main_win32.cpp.

Diese Datei laesst sich in der Entwicklungsumgebung nicht uebersetzen — es
gibt dort keine Windows-Header. Genau deshalb sind in ihr mehrfach Fehler
durchgerutscht, die in jeder anderen Datei der Uebersetzer gefunden haette:

  * eine Signatur, die nie geaendert wurde, weil die Ersetzung um ein
    Leerzeichen danebenlag und still ins Leere lief
  * Namen aus g2::gui, die im namenlosen Namensraum unqualifiziert standen

Dieses Skript faengt beide Klassen ab. Es ersetzt keinen Uebersetzer, aber es
findet genau die Fehler, die hier entstanden sind.
"""

import re
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parent.parent / "gui" / "main_win32.cpp"


def fail(msg, line=None):
    where = f" (Zeile {line})" if line else ""
    print(f"  FEHLER{where}: {msg}")
    return 1


def main():
    src = SRC.read_text(encoding="utf-8")
    errors = 0

    # 1) Namen aus g2::gui muessen qualifiziert sein.
    #
    # Alles zwischen "namespace {" und dem passenden Ende liegt ausserhalb
    # von g2::gui; die Namen der Oberflaeche gelten dort nicht ohne Praefix.
    gui_names = set()
    for header in ("icons.h", "i18n.h"):
        text = (SRC.parent / header).read_text(encoding="utf-8")
        gui_names |= set(re.findall(r"\b(?:constexpr\s+\w+|bool|void|const char\*)\s+(\w+)\s*[;(=]", text))
        gui_names |= set(re.findall(r"constexpr const char\* (\w+)", text))
    gui_names -= {"pragma", "namespace"}

    start = src.index("namespace {")
    end = src.index("}  // namespace", start)
    anon = src[start:end]

    for name in sorted(gui_names):
        for m in re.finditer(r"(?<![\w:])" + re.escape(name) + r"\b", anon):
            before = anon[max(0, m.start() - 9):m.start()]
            if before.endswith("g2::gui::"):
                continue
            line = src[:start + m.start()].count("\n") + 1
            errors += fail(f"'{name}' ohne g2::gui:: im namenlosen Namensraum", line)
            break

    # 2) Definition und Aufruf muessen in der Argumentzahl uebereinstimmen.
    defs = {}
    for m in re.finditer(r"^[\w:<>,\s\*&]+?\s(\w+)\(([^)]*)\)\s*\{", src, re.M):
        name, params = m.group(1), m.group(2)
        if name in ("if", "for", "while", "switch", "catch", "return"):
            continue
        defs[name] = 0 if not params.strip() else len(params.split(","))

    for name, want in defs.items():
        for call in re.finditer(re.escape(name) + r"\(([^;{]*?)\)\s*[;,)]", src):
            args = call.group(1)

            # Klammern muessen korrekt GESCHACHTELT sein, nicht nur gleich
            # viele.
            #
            # Bei "startupLogPath().c_str())" fing der Ausdruck ").c_str("
            # als Argumentliste ein: ein "(" und ein ")", also ausgeglichen,
            # aber in falscher Reihenfolge. Die Pruefung meldete daraufhin
            # einen Aufruf mit einem Argument, den es nicht gab.
            tiefe = 0
            kaputt = False
            for c in args:
                if c == "(":
                    tiefe += 1
                elif c == ")":
                    tiefe -= 1
                    if tiefe < 0:
                        kaputt = True
                        break
            if kaputt or tiefe != 0:
                continue

            got = 0 if not args.strip() else args.count(",") + 1
            if got != want:
                line = src[:call.start()].count("\n") + 1
                errors += fail(f"{name}() mit {got} Argumenten, definiert mit {want}", line)
                break

    # 3) Funktionen, die es erst ab Windows 10 gibt, duerfen nicht fest
    #    eingebunden werden — das Programm startet sonst auf aelteren
    #    Systemen gar nicht, mit "Einsprungpunkt nicht gefunden".
    win10_only = ["GetDpiForSystem", "GetSystemMetricsForDpi", "AdjustWindowRectExForDpi"]
    for fn in win10_only:
        for m in re.finditer(r"(?<![\w\"])" + fn + r"\s*\(", src):
            ctx = src[max(0, m.start() - 200):m.start()]
            if "GetProcAddress" in ctx or '"' + fn in src[max(0, m.start() - 40):m.start()]:
                continue
            line = src[:m.start()].count("\n") + 1
            errors += fail(f"{fn} fest eingebunden — erst ab Windows 10 vorhanden", line)
            break

    # 4) Extern deklarierte Funktionen muessen aus einer Datei kommen, die
    #    im selben CMake-Ziel liegt.
    #
    #    Genau das ging schief: main_win32.cpp rief g2cMain auf, aber
    #    tools/g2c.cpp war nicht im Ziel — weil eine Ersetzung in
    #    CMakeLists.txt nicht passte und STILL ins Leere lief. Der Fehler
    #    zeigte sich erst beim Binden unter Windows.
    cml = (SRC.parent.parent / "CMakeLists.txt").read_text(encoding="utf-8")
    gui_target = ""
    m = re.search(r"add_executable\(g2c-gui[^)]*\)", cml, re.S)
    if m:
        gui_target = m.group(0)

    for decl in re.finditer(r"^(?:int|void|bool)\s+(\w+)\([^)]*\);\s*$", src, re.M):
        fn = decl.group(1)
        if re.search(r"^[\w:<>,\s\*&]+?\s" + fn + r"\([^)]*\)\s*\{", src, re.M):
            continue   # in dieser Datei definiert
        # In welcher Datei steht die Definition?
        home = None
        for cand in (SRC.parent.parent).rglob("*.cpp"):
            if "third_party" in str(cand) or cand == SRC:
                continue
            if re.search(r"^[\w:<>,\s\*&]+?\s" + fn + r"\([^)]*\)\s*\{",
                         cand.read_text(encoding="utf-8", errors="replace"), re.M):
                home = cand
                break
        if home is None:
            errors += fail(f"'{fn}' wird deklariert, aber nirgends definiert")
        else:
            rel = home.relative_to(SRC.parent.parent).as_posix()
            if gui_target and rel not in gui_target:
                errors += fail(f"'{fn}' steht in {rel}, aber die Datei fehlt im "
                               f"CMake-Ziel g2c-gui")

    # 5) Die Symbolkennung muss in Code und Ressourcendatei gleich sein.
    rc = SRC.parent.parent / "assets" / "g2c.rc"
    if rc.exists():
        rid = re.search(r"#define\s+IDI_G2C\s+(\d+)", rc.read_text(encoding="utf-8"))
        used = set(re.findall(r"MAKEINTRESOURCEW\((\d+)\)", src))
        if rid and used and rid.group(1) not in used:
            errors += fail(f"Symbolkennung {rid.group(1)} in g2c.rc, im Code aber "
                           f"{', '.join(sorted(used))}")

    # 6a) Der angeforderte Symbolbereich muss alle benutzten Symbole
    #     enthalten. Wer eines ausserhalb hinzufuegt, saehe sonst ein leeres
    #     Kaestchen — und nur auf manchen Rechnern, weil es davon abhaengt,
    #     welche Symbolschrift installiert ist.
    icons = SRC.parent.parent / "gui" / "icons.h"
    if icons.exists():
        txt = icons.read_text(encoding="utf-8", errors="replace")
        used = [int(m, 16) for m in re.findall(r"//\s*U\+([0-9A-Fa-f]{4})", txt)]
        lo = re.search(r"kIconUsedMin\s*=\s*0x([0-9A-Fa-f]+)", txt)
        hi = re.search(r"kIconUsedMax\s*=\s*0x([0-9A-Fa-f]+)", txt)
        if used and lo and hi:
            a, b = int(lo.group(1), 16), int(hi.group(1), 16)
            out = [c for c in used if c < a or c > b]
            if out:
                errors += fail(
                    f"{len(out)} Symbol(e) ausserhalb von kIconUsedMin/Max "
                    f"(0x{a:04X}..0x{b:04X}): " +
                    ", ".join(f"U+{c:04X}" for c in sorted(out)[:5]))

    # 6c) Die Symbolfarben muessen im gueltigen Bereich liegen.
    #
    #     ImGui erwartet 0..1, nicht 0..255. Ein Wert daneben faellt nicht
    #     auf — die Farbe wird nur geklemmt und sieht falsch aus.
    if icons.exists():
        txt2 = icons.read_text(encoding="utf-8", errors="replace")
        for m in re.finditer(r"kIcon(\w+)\{([^}]*)\}", txt2):
            werte = [float(x.strip().rstrip("f")) for x in m.group(2).split(",") if x.strip()]
            if len(werte) != 4:
                errors += fail(f"kIcon{m.group(1)} hat {len(werte)} statt 4 Werte")
            elif any(v < 0.0 or v > 1.0 for v in werte):
                errors += fail(f"kIcon{m.group(1)}: Wert ausserhalb 0..1 "
                               f"(ImGui erwartet keine 0..255)")

    # 6b) Versionsangaben muessen vorhanden und in sich stimmig sein.
    #
    #     Eine Exe ohne Herkunftsangaben ist fuer Defenders Heuristik die
    #     unguenstigste Ausgangslage — sie sieht aus wie etwas, das seine
    #     Herkunft verbergen will. Das ersetzt keine Signatur, kostet aber
    #     nichts.
    rcfile = SRC.parent.parent / "assets" / "g2c.rc"
    if rcfile.exists():
        rc = rcfile.read_text(encoding="utf-8", errors="replace")
        if "VS_VERSION_INFO" not in rc:
            errors += fail("assets/g2c.rc hat keine Versionsangaben (VS_VERSION_INFO)")
        else:
            for key in ("FileDescription", "ProductName", "FileVersion", "OriginalFilename"):
                if key not in rc:
                    errors += fail(f"Versionsangabe '{key}' fehlt in assets/g2c.rc")
            # BEGIN/END muessen sich ausgleichen, sonst uebersetzt rc.exe nicht.
            if rc.count("BEGIN") != rc.count("END"):
                errors += fail(f"BEGIN/END in g2c.rc unausgeglichen: "
                               f"{rc.count('BEGIN')} zu {rc.count('END')}")

    # 6) __argv und __argc duerfen bei wWinMain nicht benutzt werden.
    #
    #    Die Laufzeitbibliothek fuellt bei einem Unicode-Einstiegspunkt nur
    #    __wargv; __argv bleibt LEER. Ein Zugriff darauf laeuft in einen
    #    Nullzeiger, und das Programm stuerzt ab, BEVOR das Fenster
    #    erscheint — von aussen sieht es aus, als passiere nichts.
    #
    #    Genau so verschwand das Ziehen einer .car auf die Exe.
    if "wWinMain" in src:
        for m in re.finditer(r"(?<![\w'\"])(__argv|__argc)\b", src):
            line_no = src[:m.start()].count("\n") + 1
            line_text = src.split("\n")[line_no - 1].lstrip()
            if line_text.startswith("//"):
                continue   # Erwaehnung im Kommentar ist in Ordnung
            errors += fail(f"{m.group(1)} bei wWinMain benutzt - ist dort leer, "
                           f"CommandLineToArgvW verwenden", line_no)
            break

    if errors:
        print(f"\n  {errors} Beanstandung(en)")
        return 1
    print("  Windows-Anbindung: keine Beanstandungen")
    return 0


if __name__ == "__main__":
    sys.exit(main())
