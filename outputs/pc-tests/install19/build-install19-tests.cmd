@echo off
rem INSTALL19: unit tests of the SkyBreak-Setup core (disc probe, JSON, progress model, build arguments, sandbox
rem platform + process allow-list, mock prerequisites, uninstall). CPU only; compiles the installer core from source.
rem   build-install19-tests.cmd            -> RESULT pass=34 fail=0   (REVINSTALL19: + C1 C2 C3 C4; REL22: + K1 K2 K3 P4)
rem   build-install19-tests.cmd --mutate   -> negative control: D4 P1 R1 S1 A2 N1 C1 C3 C4 U1 K1 K2 must FAIL
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul
cd /d "%~dp0..\..\.."
set "OUT=work\install19\unit-build"
if not exist %OUT% mkdir %OUT%
if not exist work\native-temp mkdir work\native-temp
set "TEMP=%CD%\work\native-temp"
set "TMP=%TEMP%"
set "SRC=outputs\installer\src"
set "UI=outputs\nwiirecomp\nWiiRuntime\src\ui"
cl /nologo /EHsc /std:c++20 /O1 /MT /W3 /utf-8 /DNOMINMAX /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /I%SRC% /I%UI% /Fo%OUT%\ /Fe%OUT%\install19-tests.exe ^
  outputs\pc-tests\install19\install19_tests.cpp %SRC%\common.cpp %SRC%\json.cpp %SRC%\platform.cpp %SRC%\proc.cpp %SRC%\disc_probe.cpp ^
  %SRC%\download.cpp %SRC%\prereqs.cpp %SRC%\pipeline.cpp %SRC%\engine.cpp %SRC%\headless.cpp %UI%\ui_disc.cpp %UI%\ui_wic.cpp ^
  bcrypt.lib winhttp.lib wintrust.lib crypt32.lib shell32.lib ole32.lib advapi32.lib windowscodecs.lib shlwapi.lib >%OUT%\build.log 2>&1
if errorlevel 1 (type %OUT%\build.log | findstr /C:"error" & exit /b 2)
rem REL22: the harmless test double for "SkyBreak still running" (K1-K3; also used by test_setup_flow.py / test_bouw_voortgang.py)
cl /nologo /O1 /MT /DUNICODE /D_UNICODE /Fo%OUT%\ /Fe%OUT%\dummy-skybreak.exe outputs\pc-tests\install19\dummy_skybreak.cpp /link /SUBSYSTEM:WINDOWS >>%OUT%\build.log 2>&1
if errorlevel 1 (type %OUT%\build.log | findstr /C:"error" & exit /b 2)
if "%~1"=="--mutate" (
  %OUT%\install19-tests.exe --mutate > %OUT%\mutate.log 2>&1
) else (
  %OUT%\install19-tests.exe > %OUT%\test.log 2>&1
)
set RC=%errorlevel%
if "%~1"=="--mutate" (type %OUT%\mutate.log) else (type %OUT%\test.log)
exit /b %RC%
