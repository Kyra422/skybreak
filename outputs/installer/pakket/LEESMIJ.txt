SkyBreak {version} — Alpha 2 (installer hotfix)
==========================

Community tester release for Windows x64. Experimental and unofficial.
Requires your own Skyward Sword Wii USA 1.0 (SOUE01, revision 0).

This installer update prevents overlapping builds and shows the active CMake
and Ninja output. Gameplay is unchanged from Alpha 1. If that version already
installed successfully, you can keep playing it.

INSTALL
1. Extract the entire ZIP. Keep all included folders beside Setup.
2. Run SkyBreak-Setup.exe and select your own disc dump or extracted folder.
3. Follow Setup. It checks prerequisites and asks before installing anything.
   The first build can take 20–40 minutes or longer on slower PCs.
   Cancellation and resume are supported.
4. Start SkyBreak from the desktop or Start menu.

REQUIREMENTS
Windows 10/11 x64, OpenGL 3.3 graphics, Visual Studio 2022 C++ Build Tools,
Python and DolphinTool. Setup checks the tools. Allow several GB for the game
and build output, plus approximately 7 GB if Build Tools are missing.
This installer is not digitally signed. Setup's SHA-256:
{setup_sha256}
MANIFEST.sha256 lists hashes of every included file.

CONTROLS AND FRAME RATE
Choose your layout in Settings > Controls. DualSense users can open Calibrate
gyro, rest the controller on a stable surface, collect two seconds of still
data and apply the correction. Recalibrate after restarting or reconnecting.
F1 opens settings. F8 creates a local bug report. F12 takes a screenshot.
Settings > Graphics > 60 fps (interpolated, experimental) enables smoother
presentation. Gameplay remains at 30 Hz. Performance varies by PC and scene;
native 60/144-fps gameplay is not implemented in this alpha.

TESTING
Read COMMUNITY-TESTING.md. Start without mods, then test one model mod at a
time. Rendering, motion puzzles and later game areas still need testing.
Save and close SkyBreak before updating. Setup preserves saves, settings and
local mods. Save removal is a separate optional uninstall choice.

This package contains installer/launcher stubs, our source and open-source
components. Original game files and generated game code are not included.
Your playable build is generated locally. Do not distribute that build.
License: LICENSE.txt. Dependency notices: THIRD-PARTY-NOTICES.txt.
Based on NWiiRecomp by Volodymyr Vovchok / BlackLine Interactive.
SkyBreak is not affiliated with Nintendo, ZeldaRET or Dusklight.

Advanced command-line tools are in advanced/. Use SkyBreak-Setup.exe normally.
