@echo off
setlocal
title SkyBreak - bouwen (geavanceerd)
rem INSTALL19: opdrachtregel-route. Normaal gebruik je SkyBreak-Setup.exe; dit is voor wie zelf wil bouwen.
rem Sleep je schijfbestand (.iso/.wbfs/.rvz) of uitgepakte map op dit bestand. Laat alle mappen van het pakket bij elkaar.
if "%~1"=="" goto geenschijf
echo Schijf: "%~1"
echo.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\bron\outputs\release\Bouw-SkywardSword.ps1" -Iso "%~1" -Uitvoer "%~dp0..\SkyBreak" -Werkmap "%~dp0..\SkyBreak-bouw" -ExeNaam SkyBreak-game -Product SkyBreak -Bronnen "%~dp0..\bron\work\nwii-build\_deps"
set code=%errorlevel%
echo.
if not "%code%"=="0" echo Er ging iets mis (code %code%). Maak een foto van dit venster en stuur hem op.
if "%code%"=="0" echo Klaar. Start het spel met SkyBreak\Speel.cmd
echo.
echo Druk op een toets om dit venster te sluiten.
pause >nul
exit /b %code%
:geenschijf
echo Sleep je schijfbestand (.iso, .wbfs of .rvz) of een uitgepakte map op Bouw-SkyBreak.cmd.
echo.
pause
exit /b 1
