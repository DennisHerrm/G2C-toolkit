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
268/268 Pruefungen bestanden
```
