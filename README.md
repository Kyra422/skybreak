# SkyBreak

Development previews: [Wii Remote Plus and sensor-bar support](docs/WII_REMOTE_PREVIEW.md)
is newer than the Alpha 3 installer and needs hardware retesting. Initial
[online co-op engine work](docs/ONLINE_COOP_RESEARCH.md) is experimental and does
not provide playable multiplayer.

An experimental native Windows recompilation of Skyward Sword Wii USA 1.0
(SOUE01), built locally from the player's own game. This source preview includes
the runtime, translator, installer, launcher, local achievements, Discord Rich
Presence and asset mod support. It contains no original game files or generated
game code.

## Tester release — 0.23.0 Alpha 3

[Download the Windows tester package](https://github.com/Kyra422/skybreak/releases/tag/v0.23.0-alpha.3).
Extract the entire ZIP and run **SkyBreak-Setup.exe**. The playable game is built
locally from your own supported disc; no original game files are included.

Alpha 3 includes gyro drift corrections, a controller-only adaptation of
Aurora's sensor access, immediate button remapping, Loftwing tutorial/catch
fixes, Timeshift depth rendering and interpolation improvements. See the
[release notes](docs/RELEASE_0.23.0_ALPHA3.md) and
[gyro validation](docs/GYRO_AURORA_PREVIEW.md).

This alpha includes English defaults and guides, guided gyro calibration,
controller layouts, asset mod tools, achievements and Discord Rich Presence.
Renderer fixes correct indirect-texture format bits, same-stage bump alpha and
stale GPU-copy reads during interpolation. F8 reports now show the actual build
version and use English filenames.

**Settings > Graphics > 60 fps (interpolated, experimental)** enables computed
in-between frames. Gameplay remains at 30 Hz. Native 60/144-fps gameplay is not
implemented. Performance varies by PC and scene. See
[the timing investigation](docs/TRUE_144_FPS.md).

Rendering, motion puzzles, cutscenes and later game areas still need community
testing. Read [known limitations and reporting instructions](docs/COMMUNITY_TESTING.md).

## Testing and installation

Use your own SOUE01 Wii USA 1.0 game. Extract the entire verified tester package
and run SkyBreak-Setup.exe. The first build may take time and needs Windows x64,
Visual Studio C++ Build Tools with CMake/Ninja, and OpenGL graphics support.
Setup checks dependencies and asks before installing them. It supports cancel,
resume, update and repair. Save and close the game before updating.

The source repository alone is not a prebuilt playable game. Build the installer
package using the instructions below, then supply your game locally. Never
upload your game, extracted files, save or locally built game executable.
Read [the tester guide](docs/COMMUNITY_TESTING.md) before reporting bugs.

## Building the installer package

On Windows, install Visual Studio 2022 Build Tools with C++ x64, Windows SDK and
CMake tools, plus Python 3.11+. From this repository root:

```powershell
outputs/installer/build-setup.cmd
python outputs/installer/tools/build_dist.py
```

The package appears under `outputs/vriendenpakket`. Source dependencies are
vendored under `work/nwii-build/_deps` because the existing installer build
layout uses those paths. The packager uses an explicit source allowlist and
rejects game data and console-specific secrets. Only the setup/launcher stubs
are included as executables; the game is translated and compiled on your PC.

## Contributing

Start with [CONTRIBUTING](CONTRIBUTING.md). Use narrow changes and report the
commit, input device/layout, enabled mods and reproduction steps. F8 creates a
local image/scene report; review personal information before sharing it.
Original game data and locally generated code do not belong in issues or PRs.

## Credits and license

Based on [NWiiRecomp](https://github.com/BlackLineInteractive/NWiiRecomp) by
Volodymyr Vovchok / BlackLine Interactive, with Skyward Sword symbol/configuration
research from [ZeldaRET](https://github.com/zeldaret/ss).
Preserve [the upstream modified license](outputs/nwiirecomp/LICENSE), including
its non-commercial and attribution terms. Dependencies retain their own notices;
see [CREDITS](CREDITS.md) and the installer third-party notices. No license for
original Nintendo game content is granted.

SkyBreak is unofficial and is not affiliated with Nintendo, ZeldaRET or Dusklight.
