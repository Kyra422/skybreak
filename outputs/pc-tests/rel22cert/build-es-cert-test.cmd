@echo off
rem REL22-CERT: /dev/es GetDeviceCert = generated placeholder (no real console certificate) + the ES calls SS issues.
rem Builds outputs/pc-tests/rel22cert/es-cert-test.cpp against es_device.cpp, fs_device.cpp, nand_fs.cpp, ios_kernel.cpp.
rem   build-es-cert-test.cmd            -> RESULT pass=9 fail=0
rem   build-es-cert-test.cmd --mutate   -> mutant (signed/keyed cert, retail-looking NG id, no length check): C3 C4 C5 C7 must FAIL
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul
cd /d "%~dp0..\..\.."
set "OUT=work\rel22cert\tests\bin"
if not exist work\native-temp mkdir work\native-temp
set "TEMP=%CD%\work\native-temp"
set "TMP=%TEMP%"
set "R=outputs/nwiirecomp/nWiiRuntime"
set "ES=%R%/src/hle/devices/es_device.cpp"
if "%~1"=="--mutate" (
  set "OUT=work\rel22cert\tests\mut"
  work\venv\Scripts\python.exe outputs\pc-tests\rel22cert\mk-es-cert-mutant.py work\rel22cert\tests\mut\src || exit /b 2
  set "ES=work/rel22cert/tests/mut/src/es_device.cpp"
)
if not exist %OUT% mkdir %OUT%
cl /nologo /EHsc /std:c++20 /W4 /O1 /I%R%/include /Fo%OUT%/ /Fe%OUT%/es-cert-test.exe outputs/pc-tests/rel22cert/es-cert-test.cpp %ES% %R%/src/hle/devices/nand_fs.cpp %R%/src/hle/devices/fs_device.cpp %R%/src/hle/ios_kernel.cpp >%OUT%\build.log 2>&1
if errorlevel 1 (findstr /C:"error" %OUT%\build.log & exit /b 2)
%OUT%\es-cert-test.exe > %OUT%\test.log 2>&1
set RC=%errorlevel%
findstr /b /c:"PASS:" /c:"FAIL:" /c:"RESULT" %OUT%\test.log
exit /b %RC%
