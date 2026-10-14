# Interpolation performance investigation — October 14, 2026

Interpolation remains experimental. The game runs its rules at 30 Hz; extra
presented frames do not represent native 60 Hz gameplay.

Local tests used the actual Alpha 3 executable, a copied Skyloft save, original
textures, a fixed camera, 1280x720 output and VSync. Hardware was an i9-14900T
with Radeon RX 7900 XTX. Stable 30-fps presentation was observed with
interpolation disabled. At 2x internal resolution with interpolation enabled,
the final five unsampled five-second windows were 40.0, 38.3, 41.9, 44.4 and
45.2 fps. Loading and the initial shader work are outside those windows.
This reproduces insufficient interpolation performance on this test PC.

Profiling points to CPU preparation and matching as well as rendering. In
overloaded windows, recording took roughly 7 ms and matching/planning roughly
6 ms per game frame; rendering each extra frame added roughly 11–12 ms. Lower
resolution alone did not consistently solve this workload. These measurements
do not identify the separate tester's bottleneck; their hardware, area, mods
and settings are still needed.

## First bounded optimization

The interpolation recorder previously stored another history value for every
repeated register write and each unchanged live matrix row. It now compares
the current bytes with the last stored value and records a new version only
when they differ. Per-draw sequence numbers still select the same state, and
raw comparison retains signed zero, NaN payloads and packed colour words.
Matching rules, geometry and render quality are unchanged.

A source-only workload with 6,000 live draws and repeated colour writes reduced
median recording time from 2.960 to 2.077 ms, history data from 5,570,028 to
21,164 bytes and history versions from 378,107 to 406. Every queried value in
all 107 state regions at every draw produced the same digest before and after.
This is a focused synthetic workload, not a whole-game speedup claim.

The native candidate built and loaded the private Skyloft save successfully.
All 35 interpolation regressions passed, including camera/particle guards,
per-draw state, fades, pacing, repeated writes, value reversions, signed zero,
NaN payloads and clearing history between frames.

An unsampled before/after native comparison showed variable presentation:
35.4–39.4 fps before and 35.9–50.5 after in the five final windows. The averages
were 37.22 and 41.02 fps, but this single noisy pair does not establish a
repeatable game-wide improvement or stable 60 fps. Further work should split
recording and matching costs and use repeatable camera-motion captures.

This source optimization is separate from the installer repair ZIP. That ZIP
retains the published Alpha 3 runtime. Private multiplayer experiments are not
included in the public source change or the installer repair package.

## Overload fallback follow-up — October 15, 2026

A tester reports 30 fps normally and 18 with interpolation enabled on a Ryzen 5
and RTX 2060. The exact CPU model, game area and internal resolution are still
unknown. This report is not a controlled benchmark of that hardware.

Inspection found two weaknesses in the rerender fallback. Skipping the extra
render still ran recording/matching every frame, and the work allowance grew
with the measured frame period when overload slowed the game. The fallback
now caps optional work at the game's 30-Hz allowance and suspends preparation
during its bounded retry pause. A slow initial trial or preparation-only
overrun can trigger that pause too. History is discarded during the pause and
rebuilt from fresh adjacent frames before extra frames resume. Forced tests
and the separate warp/auto paths retain their existing behavior.

In a local unsampled comparison using the same copied Skyloft save and 2x
resolution, regular renders averaged 24.70 fps before, 26.06 with the guard,
and 25.62 with interpolation off. Total presentation averaged 42.44, 31.06 and
25.62 fps respectively. The candidate skipped preparation for 708 frames over
the full run. All three runs exited normally; 41 interpolation checks passed.
The local single-player baseline also varied from the earlier 30-fps result,
so these runs establish neither a universal speedup nor steady 30/60 fps.

The moving-scene test loaded, moved Link 154 units, turned the camera 126
degrees and exited normally. Its second walking-direction check failed by
54 degrees on both the previous build and candidate, near scenery. The idle
and camera checks passed in both. This is an existing test/control limitation
to investigate separately; the complete movement suite is not reported as
passing, and no input-mapping changes are included in this patch.

This is overload protection: it sacrifices extra frames to give ordinary game
frames priority. It is not a faster 60-fps rendering implementation, and it has
not been tested on the reporter's PC. The published installer repair ZIP still
contains the original Alpha 3 runtime; this guard is a source change for a
subsequent build. Testers on that release should disable interpolation if it
reduces their frame rate.
