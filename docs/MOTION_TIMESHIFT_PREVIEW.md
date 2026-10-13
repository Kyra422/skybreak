# 0.22.0 Preview 2 — motion aiming and Timeshift rendering

Preview 2 adds changes after Graphics Preview 1. They are available on the
development branch and in the local test build, not in the Alpha 2 installer.

## Motion approach

Dusklight gets gyro/accelerometer readings through Aurora's SDL3-backed sensor
interface. Its game-specific `src/dusk/gyro.cpp` applies EMA smoothing,
deadband and gravity-aware aiming. SkyBreak already reads complete SDL2 sensor
streams and feeds original Wii MotionPlus processing; switching the whole
application to Aurora would also involve its platform/rendering dependencies.

This change adapts Dusklight's smoothing/deadband approach to SkyBreak's
existing controller pipeline. It does not claim a full Aurora integration.
Filtering uses sensor time, suppresses a small vector idle band, smooths small
aiming corrections and avoids a trailing rotation after input stops. Very slow
motion and fast movements bypass smoothing; three-axis puzzle contexts and
accelerometer data stay physical. A first attempt weakened slow-pointer input
in the original-game harness and was corrected before installation. Existing
guided calibration, high-frequency sample history and grip recentering remain.

The final code passes 14 filter checks, 114 production input checks and all 85
high-poll-rate checks using original MotionPlus/game code. The slow-pointer
measurement is 0.1943, compared with 0.1948 for the previous source, and stays
above the harness's 0.15 sensitivity requirement. These simulated tests cannot
establish how a physical controller feels to a player.

After connecting a DualSense, use **Settings > Controls > Calibrate gyro** with
the controller still, then apply calibration. Point it comfortably toward the
screen and tap the touchpad to recenter. Recalibrate after reconnecting.

## Timeshift fix

The F300 crystal activation uses GX depth textures in its compositing pass.
The previous shader ignored this feature. Preview 2 implements 8/16/24-bit
packing, replace/add, bias and 24-bit wrapping, and respects early/late depth
mode. Depth comes from the last raw texture sample before TEV channel swaps.
Bias changes update uniforms; type/operation changes select a different shader.

The GPU suite passes 46 checks. The previous shader/renderer pair passes all
previous 36 checks but fails eight depth-texture regressions. A private-save
reproduction of the reported F300 cart crystal produced the cyan flood in the
old renderer, covering 215,611 of 307,200 pixels in its worst sampled frame.
The same activation with Preview 2 displayed the transition/outline and had
no pixels meeting that solid-cyan threshold in 120 sampled frames. This is a
bounded reproduction, not proof that every Timeshift area is correct.

The activation intentionally interrupts the reused eight-slash input script;
its generic attack-count check reports a failure because the crystal cutscene
takes over after three attacks. The dedicated 114/85 control regression suites
pass. This scene run is used as a rendering reproduction, not an eight-attack
acceptance test or a performance benchmark.

Game timing is unchanged; experimental interpolation still operates over
30-Hz game logic. Original game data, private saves, screenshots/captures and
the generated game executable are kept local.
