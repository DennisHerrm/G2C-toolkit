@echo off
setlocal EnableDelayedExpansion
title g2c - Ghoul2 Toolkit Build

set "PROJECT_DIR=%~dp0"
set "PROJECT_DIR=%PROJECT_DIR:~0,-1%"
set "BUILD_DIR=%PROJECT_DIR%\build"
set "OUTPUT_DIR=%PROJECT_DIR%\output"
set "CONFIG=Release"
set "RUN_TESTS=1"

:parseargs
if "%~1"=="" goto endargs
if /i "%~1"=="debug"  set "CONFIG=Debug"
if /i "%~1"=="clean"  set "DO_CLEAN=1"
if /i "%~1"=="notest" set "RUN_TESTS=0"
if /i "%~1"=="noshell" set "NOSHELL=1"
shift
goto parseargs
:endargs

REM ===============================================================
REM  Wrapper: keeps the window open, no matter what happens inside.
REM
REM  Why this is necessary: a batch file ends IMMEDIATELY and silently
REM  when a "goto" points to a label that doesn't exist. A "pause" at
REM  the end of the file is then never reached - the window closes,
REM  and the message explaining why disappears with it.
REM
REM  "call :main" creates its own context. If something aborts inside
REM  it, execution returns here instead of closing the window. Only a
REM  bare "exit" would get through - and tools/check_batch.py verifies
REM  on every build that there is none.
REM ===============================================================
call :main %*
set "RC=%ERRORLEVEL%"

echo.
echo ------------------------------------------------------------
if "%RC%"=="0" echo  Fertig.
if not "%RC%"=="0" echo  Abgebrochen (Code %RC%) - die Meldungen oben nennen den Grund.
echo  Dieses Fenster bleibt offen. Zum Schliessen "exit" eingeben.
echo ------------------------------------------------------------
if /i "%NOSHELL%"=="1" exit /b %RC%
if exist "%OUTPUT_DIR%" cd /d "%OUTPUT_DIR%"
cmd /k
exit /b %RC%

:main
echo ============================================================
echo  g2c - Ghoul2 Toolkit  [%CONFIG%]
echo ============================================================
echo  Project:  %PROJECT_DIR%
echo ============================================================
echo.

REM ---------------------------------------------------------------
REM  Check the path for parentheses. Parentheses in the path break
REM  cmd.exe's block processing. A folder like "files(1)", as the
REM  browser creates on the second download, is enough for that.
REM ---------------------------------------------------------------
echo %PROJECT_DIR% | findstr /C:"(" >nul
if not errorlevel 1 goto badpath
echo %PROJECT_DIR% | findstr /C:")" >nul
if not errorlevel 1 goto badpath
goto pathok

:badpath
echo ERROR: Der Pfad enthaelt runde Klammern:
echo   %PROJECT_DIR%
echo.
echo Runde Klammern brechen Batch-Skripte und teilweise auch CMake.
echo Verschiebe das Projekt in einen Ordner ohne Klammern, zum Beispiel:
echo   C:\dev\g2c-toolkit
echo.
goto fail

:pathok

REM ---------------------------------------------------------------
REM  Is the project in a subfolder? Unpacking an archive easily
REM  creates one level too many, while the separately downloaded
REM  build.bat sits above it. Instead of complaining, take a look
REM  and descend on our own.
REM ---------------------------------------------------------------
if exist "%PROJECT_DIR%\CMakeLists.txt" goto rootok

if exist "%PROJECT_DIR%\g2c-toolkit\CMakeLists.txt" (
    set "PROJECT_DIR=%PROJECT_DIR%\g2c-toolkit"
    goto descended
)

for /d %%D in ("%PROJECT_DIR%\*") do (
    if exist "%%~fD\CMakeLists.txt" (
        if exist "%%~fD\include\g2\format.h" (
            set "PROJECT_DIR=%%~fD"
            goto descended
        )
    )
)
goto rootok

:descended
set "BUILD_DIR=!PROJECT_DIR!\build"
set "OUTPUT_DIR=!PROJECT_DIR!\output"
echo [OK] Projekt im Unterordner gefunden:
echo      !PROJECT_DIR!
echo.

:rootok

REM ---------------------------------------------------------------
REM  Check completeness. Downloading the files individually puts them
REM  flat into one folder - but the build needs the subfolders
REM  include\g2, src, tools and tests.
REM ---------------------------------------------------------------
set "MISSING="
if not exist "%PROJECT_DIR%\CMakeLists.txt"        set "MISSING=!MISSING! CMakeLists.txt"
if not exist "%PROJECT_DIR%\include\g2\format.h"   set "MISSING=!MISSING! include\g2\format.h"
if not exist "%PROJECT_DIR%\src\compress.cpp"      set "MISSING=!MISSING! src\compress.cpp"
if not exist "%PROJECT_DIR%\tools\g2c.cpp"         set "MISSING=!MISSING! tools\g2c.cpp"
if not exist "%PROJECT_DIR%\tests\tests.cpp"       set "MISSING=!MISSING! tests\tests.cpp"

if not "!MISSING!"=="" goto incomplete
goto complete

:incomplete
echo ERROR: Projektdateien fehlen oder liegen falsch.
echo.
echo   Nicht gefunden:!MISSING!
echo.
echo Erwartet wird diese Struktur:
echo.
echo   g2c-toolkit\
echo     build.bat
echo     CMakeLists.txt
echo     include\g2\*.h
echo     src\*.cpp
echo     tools\g2c.cpp
echo     tests\tests.cpp
echo.
echo Am einfachsten: g2c-toolkit.zip herunterladen und entpacken,
echo dann build.bat aus dem entpackten Ordner starten.
echo Einzeln heruntergeladene Dateien landen flach nebeneinander
echo und koennen so nicht gebaut werden.
echo.
echo ------------------------------------------------------------
echo  Was tatsaechlich in %PROJECT_DIR% liegt:
echo ------------------------------------------------------------
dir /b "%PROJECT_DIR%" 2>nul
echo ------------------------------------------------------------
echo.
echo Steht dort ein Ordner wie "g2c-toolkit", dann starte die
echo build.bat aus DIESEM Ordner heraus - nicht von hier.
echo.
goto fail

:complete
echo [OK] Projektdateien vollstaendig

if defined DO_CLEAN (
    if exist "%BUILD_DIR%"  rmdir /S /Q "%BUILD_DIR%"
    if exist "%OUTPUT_DIR%" rmdir /S /Q "%OUTPUT_DIR%"
    echo [OK] Aufgeraeumt
)

REM -- Find Visual Studio --
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "CMAKE_GEN=Visual Studio 17 2022"
set "HAVE_VS=0"

if not exist "%VSWHERE%" goto novs
for /f "delims=" %%R in ('"%VSWHERE%" -version [17.0^,18.0^) -property installationPath 2^>nul') do set "HAVE_VS=1"
for /f "delims=" %%R in ('"%VSWHERE%" -version [18.0^,19.0^) -property installationPath 2^>nul') do (
    set "HAVE_VS=1"
    set "CMAKE_GEN=Visual Studio 18 2026"
)
:novs

if "%HAVE_VS%"=="1" goto havevs
where g++ >nul 2>nul
if errorlevel 1 goto nocompiler
set "CMAKE_GEN=MinGW Makefiles"
set "ARCH_ARG="
echo [OK] Generator: MinGW Makefiles
goto gotgen

:nocompiler
echo ERROR: Weder Visual Studio noch g++ gefunden.
echo   Benoetigt wird ein C++20-Compiler:
echo     - Visual Studio 2022 oder neuer mit "Desktopentwicklung mit C++", oder
echo     - MinGW-w64 mit g++ im PATH
goto fail

:havevs
set "ARCH_ARG=-A x64"
echo [OK] Generator: %CMAKE_GEN%

:gotgen
where cmake >nul 2>nul
if errorlevel 1 goto nocmake
echo [OK] CMake gefunden
echo.
goto haveall

:nocmake
echo ERROR: cmake nicht im PATH.
echo   Visual Studio bringt CMake mit, aber nur in der
echo   "Developer Command Prompt". Starte build.bat von dort,
echo   oder installiere CMake separat von cmake.org.
goto fail

:haveall

REM -- Configure --
if exist "%BUILD_DIR%\CMakeCache.txt" goto configured
echo Konfiguriere...
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
cmake -S "%PROJECT_DIR%" -B "%BUILD_DIR%" -G "%CMAKE_GEN%" %ARCH_ARG% -DCMAKE_BUILD_TYPE=%CONFIG%
if errorlevel 1 goto failconfig
echo [OK] Konfiguriert
echo.
goto build

:configured
echo [OK] Build-Verzeichnis vorhanden
echo.

:build
echo ============================================================
echo  Baue Bibliothek und Kommandozeile
echo ============================================================
echo.
REM -- Only the bare essentials first. The UI is built separately, so that an
REM -- error there doesn't cost us g2c.exe.
cmake --build "%BUILD_DIR%" --config %CONFIG% --target g2c
if errorlevel 1 goto failbuild

echo.
echo [OK] g2c uebersetzt
echo.

REM -- Find binaries. MSVC puts them in a config subfolder, MinGW doesn't. --
set "BIN_DIR=%BUILD_DIR%\%CONFIG%"
if not exist "%BIN_DIR%\g2c-cli.exe" set "BIN_DIR=%BUILD_DIR%"
if not exist "%BIN_DIR%\g2c-cli.exe" goto failnobin

REM -- Deploy right away. From here on the user has a usable tool,
REM -- no matter what happens in the following steps.
if not exist "%OUTPUT_DIR%" mkdir "%OUTPUT_DIR%"
copy /Y "%BIN_DIR%\g2c-cli.exe" "%OUTPUT_DIR%\" >nul
echo [OK] Bereitgestellt: %OUTPUT_DIR%\g2c-cli.exe  (nur Kommandozeile)
echo.

echo ============================================================
echo  Baue Oberflaeche und Tests
echo ============================================================
echo.
set "GUI_OK=0"
cmake --build "%BUILD_DIR%" --config %CONFIG% --target g2c-gui
if errorlevel 1 (
  echo.
  echo [!] Die Oberflaeche liess sich nicht uebersetzen.
  echo     Die Kommandozeile g2c-cli.exe ist davon nicht betroffen und liegt bereit.
  echo.
) else (
  set "GUI_OK=1"
  if exist "%BIN_DIR%\g2c.exe" copy /Y "%BIN_DIR%\g2c.exe" "%OUTPUT_DIR%\" >nul
if exist "%BIN_DIR%\g2c-cli.exe" copy /Y "%BIN_DIR%\g2c-cli.exe" "%OUTPUT_DIR%\" >nul
  echo [OK] Oberflaeche uebersetzt und bereitgestellt
)
echo.

cmake --build "%BUILD_DIR%" --config %CONFIG% --target g2_tests
if errorlevel 1 (
  echo [!] Die Testsuite liess sich nicht uebersetzen.
  goto deploy
)

if "%RUN_TESTS%"=="0" goto deploy
echo ============================================================
echo  Testsuite
echo ============================================================
echo.
"%BIN_DIR%\g2_tests.exe"
if errorlevel 1 (
  echo.
  echo [!] Die Testsuite meldet einen Fehler.
  echo     Die Meldung oben nennt Test und Schritt.
  echo     g2c.exe (Oberflaeche + Kommandozeile) ist davon nicht betroffen.
  echo.
) else (
  echo.
  echo [OK] Alle Tests bestanden
  echo.
)

:deploy
if not exist "%OUTPUT_DIR%" mkdir "%OUTPUT_DIR%"
if exist "%BIN_DIR%\g2c.exe" copy /Y "%BIN_DIR%\g2c.exe" "%OUTPUT_DIR%\" >nul
if exist "%BIN_DIR%\g2c-cli.exe" copy /Y "%BIN_DIR%\g2c-cli.exe" "%OUTPUT_DIR%\" >nul
if exist "%BIN_DIR%\g2_tests.exe" copy /Y "%BIN_DIR%\g2_tests.exe" "%OUTPUT_DIR%\" >nul


REM --- Check: does the exe depend on the Visual C++ runtime? ------------------
REM
REM This is the most common reason why a program that was passed on doesn't
REM start for others: it reports "VCRUNTIME140.dll was not found", and the
REM recipient can do nothing about it.
REM
REM The runtime is linked statically in CMakeLists.txt (/MT). This check
REM makes sure that this actually took effect.
REM
REM Using labels instead of nested parentheses: in batch, blocks are
REM evaluated as a whole, and an "if errorlevel" inside one easily reads the
REM wrong value. Labels are longer, but they do what they say.
echo.
echo Pruefe Abhaengigkeiten...
if not exist "%OUTPUT_DIR%\g2c.exe" goto depsskip
where dumpbin >nul 2>&1
if errorlevel 1 goto depsnotool

dumpbin /dependents "%OUTPUT_DIR%\g2c.exe" > "%BUILD_DIR%\deps.txt" 2>&1
findstr /I /C:"VCRUNTIME" /C:"MSVCP" "%BUILD_DIR%\deps.txt" >nul 2>&1
if errorlevel 1 goto depsok

echo   [!!] ACHTUNG: g2c.exe braucht die Visual-C++-Laufzeit.
echo        Bei jedem ohne installiertes Redistributable startet sie NICHT.
echo        Gefunden:
findstr /I /C:"VCRUNTIME" /C:"MSVCP" "%BUILD_DIR%\deps.txt"
echo.
echo        In CMakeLists.txt muss CMAKE_MSVC_RUNTIME_LIBRARY auf
echo        "MultiThreaded" stehen und cmake_policy CMP0091 auf NEW.
goto depsdone

:depsok
echo   [OK] Laufzeit fest eingebaut - laeuft ohne Visual C++ Redistributable.
goto depsdone

:depsnotool
echo   dumpbin nicht gefunden. Diese Pruefung braucht die
echo   Entwicklereingabeaufforderung von Visual Studio; der Bau selbst
echo   ist davon nicht betroffen.
goto depsdone

:depsskip
echo   g2c.exe nicht vorhanden - nichts zu pruefen.

:depsdone
echo.

echo Bereitgestellt:
echo   %OUTPUT_DIR%\g2c.exe
if exist "%OUTPUT_DIR%\g2c.exe" echo   %OUTPUT_DIR%\g2c.exe   (Oberflaeche UND Kommandozeile - das ist die Datei zum Weitergeben)
echo.

echo Benutzung:
echo   g2c build ^<datei.car^> -ref ^<referenz.gla^> -basedir ^<pfad^>
echo             ganzes Skript abarbeiten, GLA und animation.cfg schreiben
echo             Optionen: -o -threads -carcass -skipmissing -origin
echo.
echo   g2c anim  ^<referenz.gla^> ^<ausgabe.gla^> ^<anim.xsi^> [-origin 0 0 24]
echo   g2c info  ^<datei.gla^>    Header, Skelett, Poolstatistik
echo   g2c check ^<datei.gla^>    Strukturpruefung, Fehlermessung
echo   g2c xsi   ^<datei.xsi^>    dotXSI parsen
echo   g2c car   ^<datei.car^>    Carcass-Skript auswerten
echo.
echo Dateien koennen auch direkt auf g2c.exe gezogen werden.
echo.
echo SUCCESS!
echo.

REM Keep the window open - without relying on "pause".
REM
REM "pause" waits for a key, but silently passes through when no keyboard
REM input is available. Why that happens in a given case can't be
REM determined from the outside - and a window that closes right after the
REM build swallows exactly the output you were watching the build for.
REM
REM "cmd /k" instead starts a command prompt that stays open. It can't
REM pass through, because it waits for a command rather than a key. As a
REM bonus you're already in the right folder and can try g2c right away.
REM
REM For use in scripts: "build.bat noshell" exits as usual.
exit /b 0

:failconfig
echo FAILED: CMake configure
goto fail

:failbuild
echo.
echo FAILED: Compilerfehler
goto fail

:failnobin
echo ERROR: g2c.exe nicht gefunden in
echo   %BUILD_DIR%\%CONFIG%  oder  %BUILD_DIR%
goto fail

:fail
echo.
echo BUILD FAILED - siehe Fehler oben
echo.
exit /b 1
goto :eof
