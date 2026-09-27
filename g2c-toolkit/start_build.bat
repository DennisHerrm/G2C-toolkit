@echo off
REM Starts build.bat in a way that keeps the window open no matter what.
REM
REM "cmd /k" keeps the command prompt open after the script ends -
REM regardless of whether "pause" takes effect, whether Windows wants to
REM close the window, or whether input is redirected.
REM
REM To move on: type "exit".
cmd /k ""%~dp0build.bat" %*"
