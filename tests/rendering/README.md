# Rendering regressions

This Windows x64 harness creates a hidden OpenGL 3.3 context and synthetic GX
commands/textures. It requires no game, saves, model mods or generated game code.

From a Visual Studio C++ developer terminal at the repository root:

```powershell
cmake -S tests/rendering -B build/rendering -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/rendering
ctest --test-dir build/rendering --output-on-failure
```

The 33 pixel/state checks cover TEV output, indirect texture offsets, all four
indirect bit formats, current-stage bump alpha, channel lighting and swap tables.
The final regression rewrites a GPU copy's RAM between a real and interpolated
frame: interpolation must sample the new RAM texture while preserving the real
copy registry. CTest sets `NWII_EFB_TRUECOLOR=1` to compare shader output without
the separately implemented EFB storage quantization.

The pre-fix shader fails 11 checks. Restoring the pre-fix GPU-copy handling fails
the stale-texture pixel check. Those negative controls were verified locally.
