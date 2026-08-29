@echo off
REM Startet build.bat so, dass das Fenster auf jeden Fall offen bleibt.
REM
REM "cmd /k" haelt die Eingabeaufforderung nach dem Ende des Skripts offen -
REM unabhaengig davon, ob "pause" greift, ob Windows das Fenster schliessen
REM will oder ob die Eingabe umgeleitet ist.
REM
REM Zum Weiterarbeiten: "exit" eintippen.
cmd /k ""%~dp0build.bat" %*"
