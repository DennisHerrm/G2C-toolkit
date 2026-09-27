# Ohne build.bat bauen

Falls das Skript zickt, sind das die drei Befehle dahinter. In der
**Developer Command Prompt for VS** ausfuehren, im entpackten Ordner:

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
build\Release\g2_tests.exe
```

`g2c.exe` liegt danach in `build\Release\`.

## Wenn CMake die CMakeLists.txt nicht findet

```
CMake Error: The source directory "..." does not appear to contain CMakeLists.txt
```

Dann stimmt die Ordnerstruktur nicht. So muss es aussehen:

```
g2c-toolkit\
  build.bat
  CMakeLists.txt
  include\g2\   *.h
  src\          *.cpp
  tools\        g2c.cpp
  tests\        tests.cpp
```

Einzeln heruntergeladene Dateien landen alle flach in einem Ordner. Dann
fehlen die Unterordner, und weder CMake noch der Compiler finden etwas.
Loesung: `g2c-toolkit.zip` entpacken.

## Wenn der Pfad Klammern enthaelt

```
CMake Warning: Ignoring extra path from command line
```

Ordner wie `files(1)` — die der Browser beim zweiten Download eines
gleichnamigen Archivs anlegt — enthalten runde Klammern. Die brechen die
Blockverarbeitung von cmd.exe: die schliessende Klammer beendet einen
`if (...)`-Block vorzeitig, und der Rest der Zeile landet als
zusaetzliches Argument bei CMake.

Verschiebe das Projekt an einen Pfad ohne Klammern, etwa `C:\dev\g2c-toolkit`.

## Wenn cmake nicht gefunden wird

Visual Studio bringt CMake mit, legt es aber nicht in den normalen PATH.
Entweder die **Developer Command Prompt for VS 2022** benutzen, oder CMake
separat von cmake.org installieren.

## Wenn kein Visual Studio da ist

MinGW-w64 reicht ebenfalls, ein Max SDK wird nicht gebraucht:

```bat
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build
build\g2_tests.exe
```

## Erwartete Ausgabe

```
448/448 Pruefungen bestanden
```

## Oberflaeche automatisch durchklicken

`g2_gui_driver` bedient die echte Oberflaeche ohne Fenster: Menues, alle
drei Modi, Tabelle (Klick, Strg, Umschalt, Filter, Ziehen), Kontextmenues,
Sequenzdialog, Kommentare, Pruefen, Bauen, Speichern, Schliessen, Export,
Vergleich, Vorschau, alle Sprachen, Pfade mit Umlauten. Jede
ImGui-Zusicherung und jede doppelte ID wird gemeldet.

```bat
build\Release\g2_gui_driver.exe C:\temp\g2c_gui <assetwurzel> <referenz.gla> <anims.h> <grosse.car>
```

Alles wird unter dem ersten Ordner geschrieben, `%APPDATA%` wird fuer den
Lauf dorthin umgelenkt. Die uebergebenen Dateien werden nur gelesen; die
.car wird vorher kopiert. Erwartet: `fehlgeschlagen: 0, Abstuerze: 0`
(144 Pruefungen mit Daten, 42 ohne - darunter die Update-Leiste gegen einen
nachgebauten GitHub-Server).

Nur mit dem Arbeitsordner laeuft der Teil, der keine Spieldaten braucht.
So laeuft er auch in GitHub Actions.

## Eine Version veroeffentlichen

Gebaut wird auf GitHub, nicht lokal (`.github/workflows`):

- **Jeder Push auf `main`**: bauen, alle Tests, dann die Vorabversion
  `snapshot` ersetzen.
- **Jeder Tag `v*`**: bauen, alle Tests, Release mit `g2c.exe`,
  `g2c-cli.exe`, Zip und `SHA256SUMS.txt`. Tags mit Bindestrich
  (`v1.1.0-beta`) werden Vorabversionen.

Eine neue Version:

```bat
git tag v1.0.0
git push origin v1.0.0
```

Oder auf GitHub unter *Releases > Draft a new release* einen neuen Tag
anlegen. Die Nummer folgt [Semver](https://semver.org): Fehlerbehebung
= letzte Stelle, neue Funktion = mittlere, Inkompatibles = erste.
