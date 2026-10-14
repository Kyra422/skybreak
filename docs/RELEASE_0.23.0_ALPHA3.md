# SkyBreak 0.23.0 Alpha 3

Alpha 3 brings the development previews into the Windows tester package,
including gyro drift corrections, button mapping and Loftwing tutorial fixes,
Timeshift rendering, and interpolation fixes.

## Changes

- Gyro calibration no longer learns slow gameplay turns as sensor drift.
  The Wii bridge clears old secondary corrections, and idle suppression
  covers steering and motion puzzles. Guided calibration preserves finer aim.
- Integrated a controller-only adaptation of Aurora's checked sensor access.
  Sensor activation failures retry, failed reads clear stale values, and
  accelerometer-only packets cannot repeat an old gyro turn.
- Explicit controller layouts remain selected when gyro is enabled. Captured
  button changes apply immediately; reserved shortcut handling and held-button
  release handling have been corrected.
- Switch HD action input grabs/catches during Loftwing flight and confirms
  modal tutorial messages without starting a queued flap. Ordinary flight
  retains separate flap, brake and charge actions.
- Fixed depth-texture handling implicated in the solid-cyan Lanayru Timeshift
  effect. Improved particle/camera matching, incomplete-frame rejection and
  recovery after interpolation slowdowns.
- Retains the Alpha 2 CMake logging and overlapping-build protections.

## Install or update

Extract the entire ZIP and run **SkyBreak-Setup.exe**. Supply your own
**Skyward Sword Wii USA 1.0 (SOUE01, revision 0)** disc dump or extracted folder.
Setup builds the game locally. The package contains no original game data,
generated game code, saves, console credentials, or playable game executable.

Save and close SkyBreak before updating. Choose the existing installation in
Setup; keep a backup of your own saves. The changed runtime requires rebuilding
the game. For gyro, open **Settings > Controls > Calibrate gyro**, rest the pad
on a stable surface, measure stillness, apply the correction, and test aiming.
Recalibrate after a restart or reconnect.

Compatible Wii model and texture replacements are supported. Selected Switch
HD asset mods can be converted locally; this does not provide general Switch
code-mod compatibility. Mod assets are not included.

## Known limitations

Gameplay remains at **30 Hz**. The optional 60-fps presentation uses
experimental interpolation, can produce artifacts, and may fall below 60 fps
in busy scenes. Native 60/144-fps gameplay is not implemented.

Physical controller feel, every motion puzzle, the complete Wing Ceremony,
late-game content and full-game completion have not been validated. Rendering
issues remain. This release is intended for community testing and contributors.

Press **F8** while a bug is visible. Include the version, input device/layout,
calibration state, enabled mods, interpolation setting and reproduction steps.
See the [tester guide](COMMUNITY_TESTING.md) and
[gyro implementation and validation](GYRO_AURORA_PREVIEW.md).
