#!/usr/bin/env python3
"""Static check of gui/main_win32.cpp.

This file can't be compiled in the development environment - there are no
Windows headers there. That is exactly why bugs repeatedly slipped through
in it that the compiler would have caught in any other file:

  * a signature that was never changed, because the replacement was off by
    one space and silently missed its target
  * names from g2::gui that appeared unqualified in the anonymous namespace

This script catches both classes. It doesn't replace a compiler, but it
finds exactly the bugs that occurred here.
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

    # 1) Names from g2::gui must be qualified.
    #
    # Everything between "namespace {" and its matching end lies outside
    # g2::gui; the UI's names are not valid there without a prefix.
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

    # 2) Definition and call must agree on the number of arguments.
    defs = {}
    for m in re.finditer(r"^[\w:<>,\s\*&]+?\s(\w+)\(([^)]*)\)\s*\{", src, re.M):
        name, params = m.group(1), m.group(2)
        if name in ("if", "for", "while", "switch", "catch", "return"):
            continue
        defs[name] = 0 if not params.strip() else len(params.split(","))

    for name, want in defs.items():
        for call in re.finditer(re.escape(name) + r"\(([^;{]*?)\)\s*[;,)]", src):
            args = call.group(1)

            # Parentheses must be correctly NESTED, not just equal in number.
            #
            # With "startupLogPath().c_str())" the expression captured
            # ").c_str(" as the argument list: one "(" and one ")", so
            # balanced, but in the wrong order. The check then reported a
            # call with one argument that didn't exist.
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

    # 3) Functions that only exist from Windows 10 on must not be linked
    #    statically - otherwise the program won't start at all on older
    #    systems, failing with "entry point not found".
    win10_only = ["GetDpiForSystem", "GetSystemMetricsForDpi", "AdjustWindowRectExForDpi"]
    for fn in win10_only:
        for m in re.finditer(r"(?<![\w\"])" + fn + r"\s*\(", src):
            ctx = src[max(0, m.start() - 200):m.start()]
            if "GetProcAddress" in ctx or '"' + fn in src[max(0, m.start() - 40):m.start()]:
                continue
            line = src[:m.start()].count("\n") + 1
            errors += fail(f"{fn} fest eingebunden — erst ab Windows 10 vorhanden", line)
            break

    # 4) Externally declared functions must come from a file that is in the
    #    same CMake target.
    #
    #    That is exactly what went wrong: main_win32.cpp called g2cMain, but
    #    tools/g2c.cpp was not in the target - because a replacement in
    #    CMakeLists.txt didn't match and SILENTLY missed. The error only
    #    showed up when linking on Windows.
    cml = (SRC.parent.parent / "CMakeLists.txt").read_text(encoding="utf-8")
    gui_target = ""
    m = re.search(r"add_executable\(g2c-gui[^)]*\)", cml, re.S)
    if m:
        gui_target = m.group(0)

    for decl in re.finditer(r"^(?:int|void|bool)\s+(\w+)\([^)]*\);\s*$", src, re.M):
        fn = decl.group(1)
        if re.search(r"^[\w:<>,\s\*&]+?\s" + fn + r"\([^)]*\)\s*\{", src, re.M):
            continue   # defined in this file
        # Which file contains the definition?
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

    # 5) The icon ID must be the same in the code and the resource file.
    rc = SRC.parent.parent / "assets" / "g2c.rc"
    if rc.exists():
        rid = re.search(r"#define\s+IDI_G2C\s+(\d+)", rc.read_text(encoding="utf-8"))
        used = set(re.findall(r"MAKEINTRESOURCEW\((\d+)\)", src))
        if rid and used and rid.group(1) not in used:
            errors += fail(f"Symbolkennung {rid.group(1)} in g2c.rc, im Code aber "
                           f"{', '.join(sorted(used))}")

    # 5b) Templates and types must appear BEFORE their first use.
    #
    #     main_win32.cpp can't be compiled here, so something like this only
    #     shows up on the target machine. MSVC then reports
    #     "C7568: argument list missing after assumed function template X"
    #     - a message that doesn't sound like the actual problem.
    for m in re.finditer(r"^(?:template\s*<[^>]*>\s*\n)?\s*(?:class|struct)\s+(\w+)\s*\{",
                         src, re.M):
        name = m.group(1)
        erste = None
        for u in re.finditer(r"\b" + re.escape(name) + r"\s*<", src):
            erste = u.start()
            break
        if erste is not None and erste < m.start():
            zeile = src[:erste].count("\n") + 1
            errors += fail(f"'{name}' wird in Zeile {zeile} benutzt, aber erst danach "
                           f"definiert", zeile)

    # 5d) A ComPtr must not be passed on without .get().
    #
    #     The holder deliberately does NOT silently convert to a pointer.
    #     That is correct - but then .get() is required when passing it on,
    #     and otherwise MSVC reports C2664, a message about missing
    #     conversion operators.
    #
    #     This file can't be compiled here, so otherwise it only shows up
    #     on the target machine.
    # Only check WITHIN the function in which the holder is declared.
    #
    # Searching file-wide was wrong: the same names ("dlg", "item") refer to
    # raw pointers in the older dialogs, and the check reported errors there
    # that didn't exist.
    #
    # Function boundary: the next line that starts with an identifier in
    # column 0 and contains an opening parenthesis.
    for m in re.finditer(r"\bComPtr<[^>]+>\s+(\w+)\s*;", src):
        name = m.group(1)
        rest = src[m.end():]
        naechste = re.search(r"\n[A-Za-z_][\w:<>* ]*\([^;]*\)\s*\{", rest)
        bereich = rest[: naechste.start()] if naechste else rest

        for u in re.finditer(r"[(,]\s*" + re.escape(name) + r"\s*[,)]", bereich):
            davor = bereich[max(0, u.start() - 60):u.start()]
            # In conditions the holder is allowed directly (operator bool).
            if re.search(r"(if|while|&&|\|\||return|!)\s*\(?\s*$", davor):
                continue
            zeile = src[: m.end() + u.start()].count("\n") + 1
            errors += fail(f"ComPtr '{name}' wird in Zeile {zeile} ohne .get() "
                           f"weitergereicht", zeile)
            break

    # 5c) Local buffers with the same name in the same function.
    #
    #     MSVC reports C4456 ("declaration hides previous local
    #     declaration"). Harmless as long as you notice - but in a long
    #     window procedure it's easy to grab the wrong one.
    for m in re.finditer(r"\bwchar_t\s+(\w+)\s*\[", src):
        name = m.group(1)
        treffer = re.findall(r"\bwchar_t\s+" + re.escape(name) + r"\s*\[", src)
        if len(treffer) > 2:
            errors += fail(f"'{name}' ist {len(treffer)}x als lokaler wchar_t-Puffer "
                           f"deklariert — leicht zu verwechseln")
            break

    # 6a) The requested icon range must contain all icons in use. Anyone
    #     adding one outside it would otherwise see an empty box - and only
    #     on some machines, because it depends on which icon font is
    #     installed.
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

    # 6c) The icon colors must be within the valid range.
    #
    #     ImGui expects 0..1, not 0..255. A value outside doesn't stand out -
    #     the color just gets clamped and looks wrong.
    if icons.exists():
        txt2 = icons.read_text(encoding="utf-8", errors="replace")
        for m in re.finditer(r"kIcon(\w+)\{([^}]*)\}", txt2):
            werte = [float(x.strip().rstrip("f")) for x in m.group(2).split(",") if x.strip()]
            if len(werte) != 4:
                errors += fail(f"kIcon{m.group(1)} hat {len(werte)} statt 4 Werte")
            elif any(v < 0.0 or v > 1.0 for v in werte):
                errors += fail(f"kIcon{m.group(1)}: Wert ausserhalb 0..1 "
                               f"(ImGui erwartet keine 0..255)")

    # 6b) Version information must be present and self-consistent.
    #
    #     For Defender's heuristics, an exe without origin information is
    #     the worst possible starting point - it looks like something that
    #     wants to hide where it came from. This doesn't replace a signature,
    #     but it costs nothing.
    rcfile = SRC.parent.parent / "assets" / "g2c.rc"
    if rcfile.exists():
        rc = rcfile.read_text(encoding="utf-8", errors="replace")
        if "VS_VERSION_INFO" not in rc:
            errors += fail("assets/g2c.rc hat keine Versionsangaben (VS_VERSION_INFO)")
        else:
            for key in ("FileDescription", "ProductName", "FileVersion", "OriginalFilename"):
                if key not in rc:
                    errors += fail(f"Versionsangabe '{key}' fehlt in assets/g2c.rc")
            # BEGIN/END must balance, otherwise rc.exe won't compile it.
            if rc.count("BEGIN") != rc.count("END"):
                errors += fail(f"BEGIN/END in g2c.rc unausgeglichen: "
                               f"{rc.count('BEGIN')} zu {rc.count('END')}")

    # 6) __argv and __argc must not be used with wWinMain.
    #
    #    With a Unicode entry point, the runtime library only fills
    #    __wargv; __argv stays EMPTY. Accessing it hits a null pointer, and
    #    the program crashes BEFORE the window appears - from the outside it
    #    looks as if nothing happens.
    #
    #    That is exactly how dragging a .car onto the exe stopped working.
    if "wWinMain" in src:
        for m in re.finditer(r"(?<![\w'\"])(__argv|__argc)\b", src):
            line_no = src[:m.start()].count("\n") + 1
            line_text = src.split("\n")[line_no - 1].lstrip()
            if line_text.startswith("//"):
                continue   # a mention in a comment is fine
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
