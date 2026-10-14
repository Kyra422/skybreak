@echo off
setlocal
title SkyBreak - build (advanced)
rem INSTALL19: opdrachtregel-route. Normaal gebruik je SkyBreak-Setup.exe; dit is voor wie zelf wil bouwen.
rem Sleep je schijfbestand (.iso/.wbfs/.rvz) of uitgepakte map op dit bestand. Laat alle mappen van het pakket bij elkaar.
if "%~1"=="" goto geenschijf
echo Disc: "%~1"
echo.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\bron\outputs\release\Bouw-SkywardSword.ps1" -Iso "%~1" -Uitvoer "%~dp0..\SkyBreak" -Werkmap "%~dp0..\SkyBreak-bouw" -ExeNaam SkyBreak-game -Product SkyBreak -Bronnen "%~dp0..\bron\work\nwii-build\_deps"
set code=%errorlevel%
echo.
if not "%code%"=="0" echo Build failed (exit %code%). Include this output when reporting the issue.
if "%code%"=="0" echo Done. Start the game with SkyBreak\Play.cmd
echo.
echo Press a key to close this window.
pause >nul
exit /b %code%
:geenschijf
echo Drop your disc (.iso, .wbfs or .rvz) or extracted folder onto Bouw-SkyBreak.cmd.
echo.
pause
exit /b 1
