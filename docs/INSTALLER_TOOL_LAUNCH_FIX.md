# Alpha 3 tool-launch failure

The error `Cannot start process because a file name has not been provided`
at `Bouw-SkywardSword.ps1` during **Checking tools** occurs before CMake runs.
The script passed Windows' `ComSpec` environment variable directly to process
creation when initializing MSVC. An absent variable reproduces the reported
exception with the original Alpha 3 script. The tester's environment has not
been inspected, so the exact cause on that PC still needs a retry confirmation.

The build script now falls back to `cmd.exe` in Windows' system directory for
both direct tool checks and logged builds. It rejects empty program names,
prints the executable before launching it, and includes the executable and
current step in direct-launch failures. Python launcher discovery selects
applications rather than PowerShell functions or aliases.

## Installer repair package

[Download the repaired Alpha 3 installer](https://github.com/Kyra422/skybreak/releases/download/v0.23.0-alpha.3/SkyBreak-Alpha3-Installer-Fix.zip).
It is published alongside the original assets on the Alpha 3 release page.
Use `SHA256SUMS-installer-fix.txt` to verify this download.

A repair ZIP can retain the published Alpha 3 game sources and installer
executables while replacing only the build script, regenerating its integrity
manifest, and adding English instructions. It must not silently overwrite the
original published asset. Extract the complete repaired ZIP to a new folder,
start its Setup, and choose the same installation/build folders to resume.
Do not delete completed builds or saves. Editing only the installed script is
insufficient: Setup synchronizes sources again when retrying from an old ZIP.

## Validation

`python tests/test_build_tools.py -v` runs six Windows regressions: absent and
stale `ComSpec`, empty direct/logged program names, a missing executable, and
the actual Python/MSVC/CMake/Ninja prerequisite step without `ComSpec`.
The Windows CI job runs these checks too.

The local repair package also passed Setup's own manifest verification and
the packaged script's tool and supported-disc checks without `ComSpec`.
Byte comparison confirmed all game/runtime files and installer executables
remain identical to the original Alpha 3 ZIP. Game-data and console-data scans
passed. A new complete game compilation was not repeated for this script-only
repair; the affected prerequisite check and process-launch paths were exercised.
