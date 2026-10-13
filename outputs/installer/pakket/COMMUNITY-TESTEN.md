# SkyBreak 0.22.0 Alpha 2 — community testing

Use your own **Skyward Sword Wii USA 1.0 (SOUE01, revision 0)**. Extract the
whole archive and run `SkyBreak-Setup.exe`. Setup checks tools and requests
consent before installing missing prerequisites. Cancel/resume is supported.
Save and close SkyBreak before updating; Setup waits for the game to close.

Alpha 2 updates the installer: concurrent builds are blocked, the active
CMake/Ninja output appears in the log view, and **Open build log** opens the
actual diagnostic file. Gameplay is unchanged from Alpha 1; a successful
Alpha 1 installation does not need rebuilding.

## First session

1. Start with factory settings and no mods. Confirm saving and reloading work.
2. Choose your layout in **Settings > Controls**. Test aiming, menus and sword
   movement with your usual input device.
3. For DualSense gyro, open **Calibrate gyro**, rest the pad on a stable surface,
   collect two seconds of still data, apply and test the pointer. Calibration
   is specific to the current connection/session. Recalibrate after restarting
   or reconnecting. Keyboard/mouse and pads without gyro use virtual motion.
4. Try **Settings > Graphics > 60 fps (interpolated, experimental)**. Compare
   both settings when reporting glitches. Computed frames smooth presentation;
   game logic remains at 30 Hz. Native 60/144-fps gameplay is not implemented.
5. Test one complete Link model mod at a time, then restart. Compatible Wii
   assets work directly; selected HD BRRES+XTX assets need local conversion.
   ObjectPack replacements are not merged. Switch code patches are unsupported.

## Known limitations

This is an alpha, not a validated full playthrough. Rendering artifacts, blur
and bright lava in Eldin/Earth Temple are under investigation. Interpolation
can show artifacts during effects, camera changes or text transitions, and
can fall back to a lower frame rate in busy scenes. Physical controller feel,
all motion puzzles, cutscenes and late-game equipment are not fully validated.

## Reporting a bug

Press **F8 while the issue is visible**. The local `reports` folder contains
`screenshot.png`, `context.txt` and a diagnostic ZIP with settings and recent
logs. Fill in the description, then report it on
[GitHub Issues](https://github.com/Kyra422/skybreak/issues) or the project Discord.
Include version, Windows/GPU, input device/layout, enabled mods, interpolation
and HD texture settings, exact route and steps, and whether it repeats.
For a flickering issue, attach a short video as well as a still image.

Review files for personal paths before sharing. Do not upload original game
files, saves, memory captures, copyrighted mod assets or the locally built
game executable. Keep a backup of your own save folder before testing.
