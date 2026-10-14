# SkyBreak 0.22.0 Alpha 1

First public Windows community tester package. Requires your own **Skyward
Sword Wii USA 1.0 (SOUE01, revision 0)**. Extract the whole ZIP and run
**SkyBreak-Setup.exe**. Setup checks dependencies and builds your playable
version locally; the first build can take 20–40 minutes or longer.

## Included

- English installer and launcher defaults, tester guide and error messages.
- Guided physical gyro calibration and several controller/keyboard layouts.
- Optional **60-fps interpolated presentation** in Settings > Graphics.
- Asset mod management and local model conversion tools, achievements and
  Discord Rich Presence.
- Renderer corrections for indirect-texture bit formats, current-stage bump
  alpha and stale GPU-copy reads during interpolation.
- F8 image/context reports with the correct version label and English filenames.
- Verified dependency downloads, package checksums, cancel/resume, update/repair
  and preservation of personal saves, settings and mods.

## Before testing

Start without mods and keep your saves backed up. Gameplay runs at **30 Hz**;
the 60-fps option computes extra displayed frames. Native 60/144-fps gameplay
is not available. Rendering artifacts, Eldin/Earth Temple blur/bright effects,
some mod facial states and motion puzzles still need investigation. This alpha
is not a validated full playthrough and does not promise a fixed frame rate.

Windows x64 and OpenGL 3.3 are required. Setup is currently unsigned.
Report issues using F8 plus the route, input device, enabled mods and graphics
settings. Review reports for personal paths before sharing.

The ZIP contains setup/launcher stubs, project source and open-source components.
It contains no original game files, generated game code, playable game executable,
saves or character-mod assets. Do not distribute your locally built game folder.

[Tester guide](https://github.com/Kyra422/skybreak/blob/v0.22.0-alpha.1/docs/COMMUNITY_TESTING.md)
· [Report a bug](https://github.com/Kyra422/skybreak/issues)

Based on NWiiRecomp by Volodymyr Vovchok / BlackLine Interactive and symbol/config
research from ZeldaRET. Preserve the included licenses and dependency notices.
SkyBreak is unofficial and unaffiliated with Nintendo, ZeldaRET or Dusklight.
