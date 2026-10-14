# Wii Remote Plus development preview

This preview adds a dedicated real Wii Remote path and physical sensor-bar IR
input for **normal Bluetooth**.
It is newer than the published `v0.23.0-alpha.3` installer. Installing the
published Alpha 3 ZIP alone does not include this preview.

## Connect on Windows

1. Attach a Nunchuk to your Wii Remote Plus before connecting.
2. Turn on your PC's Bluetooth and close Dolphin or other apps using the remote.
3. In SkyBreak, open **F1 → Settings → Controls → Remap buttons...**.
4. Press the red **SYNC** button inside the remote's battery compartment.
5. Select **Pair Wii Remote**, keeping its LEDs flashing during the search.
6. Wait for SkyBreak to show a connected Wii Remote and active MotionPlus input.

For physical pointing, place a powered sensor bar in front of the display. An
ordinary Wii/Wii U sensor bar powered by its console can supply the lights;
the Wii Remote stays connected to the PC. Point at the bar and check that the
controls page reports **Sensor bar: visible**. Press **Down** while pointing at
the screen centre to establish the pointing reference. Gyro calibration is separate.

The pairing helper uses Windows' Bluetooth HID service and the SYNC binary PIN.
It runs on a worker thread and only targets the Nintendo Wii Remote device names.
It does not remove Bluetooth devices or change adapter drivers. A pairing request
that succeeds is not proof that SkyBreak has received buttons or sensor reports.

The native path activates independently of Xbox/PlayStation/Switch presets and
the gamepad gyro toggle. Wii A remains A, Wii B remains B, and all original
remote buttons retain their meanings. The Nunchuk supplies its stick, C/Z buttons,
and accelerometer. Full MotionPlus movement supplies the sword and motion puzzles.
Press **Down** to centre the pointer. Use **Calibrate gyro...** if there is drift:
place the remote still, collect the sample, then apply the correction.

## Scope and verification

- Ordinary Bluetooth Wii Remote Plus and Nunchuk are the intended hardware.
- Physical IR is decoded from report `0x37` alongside accelerometer and six
  MotionPlus/Nunchuk extension bytes. The driver enables the IR camera in basic
  mode and uses Wii sensitivity level 3. At least two visible points establish
  the bar centre and horizon. Lost/stalled IR reports invalidate physical pointing;
  the existing gyro path remains available. The Wii Remote speaker, DolphinBar
  transport, and Classic Controllers are outside this preview.
- The driver also fixes a real upstream report-decoding issue: reports lacking
  accelerometer bytes must not generate a fabricated acceleration sample. Report
  `0x37` now explicitly retains its real accelerometer bytes.
- SDL's sensor axes and units are converted to the existing guest MotionPlus path.
  Gamepad grip offsets and gravity-relative aim are not applied to a real remote.
- Untimed Wii reports use monotonic arrival times, so a fast host poll does not
  shrink physical rotation intervals or repeatedly integrate stale angular rates.
- Failed reads, disconnected devices, and focus loss release input. The real
  remote owns aiming while connected, avoiding accidental desktop-mouse takeover.
- Windows native build and private-save loading passed. The pairing panel was
  opened and captured in the real game. Real save, settings, and mods stayed unchanged.
- 55 production Wii input checks and all 185 existing production controller checks
  passed. Tests cover exact buttons in every saved preset, Nunchuk inputs, three-axis
  motion, untimed reports, idle host polls, sensor stalls, reconnects, focus loss,
  physical IR routing, bar loss, packet stalls, and pointing-reference resets.
- 25 extracted production-driver checks pass for IR decoding, report layout,
  acceleration guards, initialization, and initialization failures. The old driver
  reproduces three failures related to absent acceleration. Both original-game
  motion/sword harness runs pass all 100 checks each.
- Initial Bluetooth pairing succeeded on the user's Wii Remote Plus with Nunchuk;
  the game log confirms 100 Hz sensors and native MotionPlus/Nunchuk activation.
  The user subsequently reported poor aiming **and swings** in the first revision.
  Connection success did not establish acceptable motion quality.
- **The new sensor-bar path and acceleration fix still need physical retesting.**
  Check bar visibility, pointer direction, return to rest, all sword directions,
  Nunchuk gestures, the Beetle, and motion puzzles. A full playthrough is unverified.

The implementation reuses the pinned [SDL 2.28.5 Wii HIDAPI driver](https://github.com/libsdl-org/SDL/blob/release-2.28.5/src/joystick/hidapi/SDL_hidapi_wii.c).
SDL disables this driver by default because of DolphinBar incompatibility; this
preview explicitly enables it for ordinary Bluetooth. `NWII_WII_REMOTE=0` disables
Wii discovery for troubleshooting. Windows pairing protocol references were checked
against [Dolphin's Windows Wii Remote backend](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/HW/WiimoteReal/IOWin.cpp).
