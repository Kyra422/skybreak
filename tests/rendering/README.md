# Rendering regressions

This Windows x64 harness creates a hidden OpenGL 3.3 context and synthetic GX
commands/textures. It requires no game, saves, model mods or generated game code.

From a Visual Studio C++ developer terminal at the repository root:

```powershell
cmake -S tests/rendering -B build/rendering -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/rendering
ctest --test-dir build/rendering --output-on-failure
```

The 46 pixel/state checks cover TEV output, indirect texture offsets, all four
indirect bit formats, current-stage bump alpha, channel lighting and swap tables.
The final regression rewrites a GPU copy's RAM between a real and interpolated
frame: interpolation must sample the new RAM texture while preserving the real
copy registry. CTest sets `NWII_EFB_TRUECOLOR=1` to compare shader output without
the separately implemented EFB storage quantization. The shader-budget regression
refuses an incomplete interpolated frame, checks that the real EFB is preserved,
and accepts the frame once its shader is available.

The pre-fix shader fails 11 checks. Restoring the pre-fix GPU-copy handling fails
the stale-texture pixel check. The renderer before the completeness fix fails
the incomplete-frame rejection check. Those negative controls were verified locally.

Depth-texture checks cover 8/16/24-bit packing, replace/add, bias and wrapping,
early-depth behavior, raw sampling before TEV channel swaps, dynamic bias updates,
and a cyan overlay rejected behind nearer geometry. The previous renderer/shader
pair fails eight of these checks and passes all previous 36 checks.

The source-only `tests/interpolation_test.cpp` contains 31 checks and runs in CI
without game data or a GPU. Its particle regressions cover a dominant burst of
ambiguous direct-position draws, world-camera estimation, holding an uncertain
effect's current geometry, and retaining indexed-geometry interpolation. The
previous header fails those three particle regressions and passes the other 28.
