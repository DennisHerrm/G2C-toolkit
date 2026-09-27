@echo off
REM ---------------------------------------------------------------
REM  Test: does a window stay open at all?
REM
REM  This file does nothing but pause. If it still closes right away
REM  on double-click, the cause is NOT build.bat but the file
REM  association for .bat in Windows.
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
