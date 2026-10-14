@echo off
rem Skyward Sword PC - dubbelklik om te spelen. Opties voor een keer, bijvoorbeeld:
rem   Speel.cmd -Schaal 3 -Volledig      (zie LEESMIJ.txt)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0hulp\Speel.ps1" %*
if errorlevel 1 pause
