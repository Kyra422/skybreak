# Building SkyBreak

See the repository README for the installer-package build. The setup takes your
own SOUE01 disc image or extracted game folder and runs
`outputs/release/Bouw-SkywardSword.ps1` on your PC. No original game data is
included. The script checks the game revision, translates DOL/REL code and builds
the native executable locally. Building the setup stubs does not build the game.

For a standalone calibration regression, open an x64 Visual Studio developer
prompt from this repository root:

```text
cl /EHsc /std:c++20 /O2 /Ioutputs/nwiirecomp/nWiiRuntime/src /Ioutputs/nwiirecomp/nWiiRuntime/include tests/gyro_calibration_test.cpp /Fe:gyro-test.exe
gyro-test.exe
```

Do not commit local build artifacts, generated game code or game files. The
packager reports checksums and rejects known game/console data. Full package
install/resume/update/uninstall validation uses an isolated local test folder;
this is not proof of compatibility on every fresh Windows machine.
