# SkyBreak execution and rendering

SkyBreak translates PowerPC game functions into native C++ ahead of time and
uses a CPU interpreter fallback for code that is not translated or resolved.
The number of displayed frames is separate from the guest game's update rate.
The current release keeps game logic at 30 Hz. Its optional 60-fps mode renders
computed intermediate frames; it does not implement native 60/144-fps gameplay.

The runtime currently decodes GX commands and renders them with its OpenGL
backend. See `outputs/nwiirecomp/nWiiRuntime/src/hle/gx/renderer_factory.cpp`
and `renderer_gl.cpp`. Source-only synthetic rendering regressions are in
`tests/rendering`; they do not require original game assets.

## Aurora investigation

[Aurora](https://github.com/encounter/aurora) is a source-level GameCube/Wii
compatibility library. Its documented renderer uses Dawn/WebGPU to target
D3D12, Vulkan and Metal; it also supplies pipeline caching, texture packs and
controller/gyro input. It is used by Dusklight.

Aurora is **not integrated into this release**. Our assessment is that it may
help graphics correctness and portability, but adapting SkyBreak's decoded GX
command/state pipeline would require a separate backend experiment and scene
comparisons. It would not automatically remove CPU fallback or correct the
game's per-frame physics, animations and timers.

References: [Aurora features](https://github.com/encounter/aurora#readme),
[library integration](https://github.com/encounter/aurora/blob/main/docs/building.md).
