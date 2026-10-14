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
