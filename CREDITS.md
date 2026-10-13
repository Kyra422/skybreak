# Credits and provenance

## Starting research baseline

- [zeldaret/ss](https://github.com/zeldaret/ss): Skyward Sword Wii decompilation and reverse-engineering work.
- [Previously inspected baseline](https://github.com/zeldaret/ss/tree/83a06ced84253a3136210f4d43f1e50f1f1303b2): `83a06ced84253a3136210f4d43f1e50f1f1303b2`.
- Configured target of that baseline: `SOUE01`, Wii NTSC-U 1.0 (USA).

This records the earlier research baseline. It does not establish the source commit of an uninspected native implementation.

## Documentation reference

[Dusklight](https://github.com/TwilitRealm/dusklight) inspired the README's restrained presentation and section structure. This package includes no Dusklight logo, screenshots, runtime code, or dependency branding. Inspiration does not imply collaboration or endorsement.

## Dependencies and contributors

As code is integrated, record each dependency's upstream URL, exact revision, purpose, license, and notice location. Preserve existing notices and contributor history. Do not list Aurora or any other runtime as a Skybreak dependency until it is actually adopted.

Original game content and trademarks belong to their respective rights holders and are not licensed by this project.

## Integrated native source preview

NWiiRecomp by Volodymyr Vovchok / BlackLine Interactive is the translator/runtime
baseline. Its license is preserved in outputs/nwiirecomp/LICENSE. SDL2 2.28.5
(zlib), toml++ 3.4.0 (MIT), Dear ImGui 1.91.9b (MIT), and the bundled third-party
components retain their source notices. Controller glyph stencils use Kenney
Input Prompts (CC0), with their recipe and provenance preserved. See
outputs/installer/pakket/DERDEN-LICENTIES.txt for the dependency notice index.
Optional texture2ddecoder 1.0.6 (MIT) is installed separately for HD asset authoring
and is not vendored. No third-party character mod or game texture ships here.

NWiiRecomp checkout baseline: `595f176f1d24cc54ff2e8389feed12d7fb553cc2` from BlackLineInteractive/NWiiRecomp. This preview includes local SkyBreak adaptations; it is not an unmodified upstream snapshot.

Motion aiming adapts the EMA/deadband approach in Dusklight's
[`src/dusk/gyro.cpp`](https://github.com/TwilitRealm/dusklight/blob/34a144a493b7f57adeb4034d382317c826eb5c8d/src/dusk/gyro.cpp),
revision `34a144a493b7f57adeb4034d382317c826eb5c8d`, under
[CC0-1.0](https://github.com/TwilitRealm/dusklight/blob/34a144a493b7f57adeb4034d382317c826eb5c8d/LICENSE.md).
The adaptation uses sensor timestamps and preserves the Wii MotionPlus path.
Aurora's SDL3 sensor interface was inspected as a reference; Aurora itself is
not linked into this runtime.
