SkyBreak {version}
==================

SkyBreak is a PC port of The Legend of Zelda: Skyward Sword, built from YOUR
OWN disc. SkyBreak is a fan project and not an official Nintendo product.
Non-commercial.

This package contains NO game code, NO game files and NO textures: only the
installer, the translator and runtime (source code), the build script and
open-source libraries. Everything derived from the disc is made on your PC
only. Do not share the folder that Setup builds.

WHAT'S NEW IN 0.21.0
--------------------
- Guided gyro calibration and pointer preview; gravity-relative aiming.
- Verified official DolphinTool download and pinned Python SHA-256.
- Mod authoring tools installed automatically; read COMMUNITY-TESTEN.md.
- True 144 fps is not implemented.

WHAT'S NEW IN 0.20.0
--------------------
- Launcher: SkyBreak opens on a main screen with a film and music
  (PLAY / SETTINGS / EXIT). Settings in 7 tabs with explanations, reset and
  "Save changes?"; everything works with mouse, keyboard or controller.
- Controls: layouts "Switch HD", "Classic", "Wii (motion/gyro)" and
  "Keyboard + mouse"; rebind a button by pressing it; a test page; button
  icons for Xbox, PlayStation, Switch and keyboard.
- Free camera: turn the camera around Link (right stick or mouse); let go
  and it stays where you left it. Off = exactly the Wii camera.
- Background film on the main screen (from your own disc), music at 60 %.
- Shutdown: SkyBreak now closes cleanly; the crash on exit is gone.
- Setup: asks you to close SkyBreak before it updates, repairs or
  uninstalls (instead of stopping halfway). Folders with accents or other
  characters (for example C:\Users\Zoë) now work. The game has the
  SkyBreak icon and the launcher the wing logo. The button icons are made on
  your PC after the build (Kenney artwork, CC0, fitted using your own disc).

INSTALL
-------
1. Extract the WHOLE zip (right-click > Extract All).
2. Double-click SkyBreak-Setup.exe.
   Windows SmartScreen may say "Windows protected your PC": click
   "More info" > "Run anyway". Setup is not digitally signed (yet). Feel free
   to check the SHA-256 below.
3. Follow the steps:
   - Welcome: confirm that you use a dump of your own copy.
   - Disc: choose your .iso / .wbfs / .rvz / .wia / .gcz or an extracted
     folder. Only the American version 1.00 (SOUE01, revision 0) works.
   - Folders: default %LOCALAPPDATA%\Programs\SkyBreak (no administrator
     needed), saves in %APPDATA%\SkywardSwordPC\nand (same folder as before).
   - Prerequisites: Visual Studio 2022 Build Tools (C++), Python and
     DolphinTool. Whatever is missing is only installed with your consent.
   - Build: about 20-40 minutes, with a progress bar. Cancelling is fine;
     Setup continues later where it stopped.
4. Start SkyBreak from the Start menu or desktop shortcut.

REQUIREMENTS
------------
- Windows 10 or 11, 64-bit; a graphics card with OpenGL 3.3.
- About 6 GB of free space (2.4 GB when you point to an extracted folder),
  plus about 7 GB if Visual Studio still has to be installed.

UPDATE AND UNINSTALL
--------------------
- New version: extract the new zip and start SkyBreak-Setup.exe. Setup finds
  your installation, replaces only changed source files and builds
  incrementally (about 10 minutes). Saves and settings stay. If SkyBreak
  is still running, Setup asks you to close it first.
- Uninstall: Windows Settings > Apps > SkyBreak > Uninstall. Your saves are
  kept unless you tick that separately and confirm.

ADVANCED
--------
The geavanceerd\ folder has the command-line route (Bouw-SkyBreak.cmd and
Installeer-Benodigdheden.cmd) for people who prefer to build by hand.

VERIFY
------
SHA-256 of SkyBreak-Setup.exe: {setup_sha256}
MANIFEST.sha256 lists the SHA-256 of every file; Setup checks it before
installing.

LICENCES
--------
LICENTIE.txt (NWiiRecomp licence: non-commercial, attribution, no AI
training) and DERDEN-LICENTIES.txt (SDL2, toml++, Dear ImGui, Dolphin
signatures, decomp configuration).
Based on NWiiRecomp by Volodymyr Vovchok (BlackLine Interactive).
The Legend of Zelda: Skyward Sword is a trademark of Nintendo.
