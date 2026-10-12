@echo off
rem INSTALL19: builds SkyBreak-Setup.exe + SkyBreak.exe (+ sb_core for the tests) into work\install19\build.
rem In this project: call through the build lock, e.g.
rem   LOCK_MAX_WAIT=300 bash work/claude-tools/locked.sh install19 cmd //c outputs\installer\build-setup.cmd
rem Needs Visual Studio 2022 (or Build Tools) with C++ and CMake tools. Downloads nothing.
setlocal
set "HERE=%~dp0"
for %%I in ("%HERE%..\..") do set "ROOT=%%~fI"
set "OUT=%ROOT%\work\install19\build"
if not "%~1"=="" set "OUT=%~1"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSDIR="
if exist "%VSWHERE%" for /f "usebackq delims=" %%V in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%V"
if not defined VSDIR (echo [build-setup] Visual Studio with C++ not found & exit /b 2)
call "%VSDIR%\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 -no_logo >nul
if errorlevel 1 (echo [build-setup] VsDevCmd failed & exit /b 2)
rem CMake/Ninja: from Visual Studio's "C++ CMake tools" when present, else the project's venv (pip cmake/ninja)
where cmake >nul 2>&1 || set "PATH=%ROOT%\work\venv\Scripts;%PATH%"
where ninja >nul 2>&1 || set "PATH=%ROOT%\work\venv\Scripts;%PATH%"
where cmake >nul 2>&1 || (echo [build-setup] cmake not found & exit /b 2)
if not exist "%ROOT%\work\native-temp" mkdir "%ROOT%\work\native-temp"
set "TEMP=%ROOT%\work\native-temp"
set "TMP=%TEMP%"
cmake -S "%HERE%." -B "%OUT%" -G Ninja -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b %errorlevel%
cmake --build "%OUT%" -j 8
if errorlevel 1 exit /b %errorlevel%
echo [build-setup] OK: %OUT%\SkyBreak-Setup.exe %OUT%\SkyBreak.exe
exit /b 0
