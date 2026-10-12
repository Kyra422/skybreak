@echo off
rem INSTALL19: one command for the distributable: builds SkyBreak-Setup.exe + SkyBreak.exe (build-setup.cmd) and then
rem the package outputs\vriendenpakket\SkyBreak-<VERSION>.zip (tools\build_dist.py: allow-list copy, legal scan that
rem refuses any game-derived file, MANIFEST.sha256, zip, unpack + scan again). Downloads nothing, installs nothing.
rem In this project: call through the build lock, e.g.
rem   LOCK_MAX_WAIT=300 bash work/claude-tools/locked.sh install19 cmd //c outputs\installer\build-package.cmd
setlocal
set "HERE=%~dp0"
for %%I in ("%HERE%..\..") do set "ROOT=%%~fI"
call "%HERE%build-setup.cmd"
if errorlevel 1 (echo [build-package] build-setup failed & exit /b 2)
set "PY=%ROOT%\work\venv\Scripts\python.exe"
if not exist "%PY%" set "PY=py -3"
%PY% "%HERE%tools\build_dist.py"
if errorlevel 1 (echo [build-package] build_dist failed ^(legal scan refused or packaging error^) & exit /b 3)
echo [build-package] OK: %ROOT%\outputs\vriendenpakket
exit /b 0
