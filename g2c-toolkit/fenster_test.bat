@echo off
REM ---------------------------------------------------------------
REM  Test: bleibt ein Fenster ueberhaupt offen?
REM
REM  Diese Datei tut nichts ausser anzuhalten. Geht sie beim
REM  Doppelklick trotzdem sofort zu, liegt es NICHT an build.bat,
REM  sondern an der Dateizuordnung fuer .bat in Windows.
REM ---------------------------------------------------------------
echo.
echo Wenn du das hier lesen kannst, ist alles in Ordnung.
echo.
echo ------------------------------------------------------------
echo  1) pause
echo ------------------------------------------------------------
pause
echo.
echo ------------------------------------------------------------
echo  2) pause hat funktioniert. Jetzt cmd /k.
echo     Zum Schliessen "exit" eingeben.
echo ------------------------------------------------------------
cmd /k
