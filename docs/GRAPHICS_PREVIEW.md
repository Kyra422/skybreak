# 0.22.0 Graphics Preview 1

This development branch includes graphics changes after the published Alpha 2
installer. The Alpha 2 download does not contain these changes. The launcher and
F8 reports identify builds from this source as **0.22.0 Graphics Preview 1**.

## Changes

- Ambiguous direct-position effects no longer vote for the world-camera
  transform. A large particle burst could otherwise make that estimate jump
  between the actual camera turn and an identity transform.
- Ambiguous effect matches keep their current geometry. Blending two unrelated
  particle packets could pull geometry between different world positions.
  Reliable camera motion and indexed-geometry interpolation remain enabled.
- An interpolated frame with missing shader draws is rejected after restoring
  the real-frame state. This prevents presenting a partially rendered image
  between complete frames while new shaders compile.
- The render-budget guard retries sooner after a small slowdown. Heavier
  overloads still extend the pause, and the pending-real-frame guard remains on.
  Preparation was already included in the measured interpolation duration;
  the prediction now counts it once instead of adding it again.

## Validation and limits

The source-only interpolation suite passes 31 checks. The previous header fails
the three new particle/camera regressions and passes the other 28. The synthetic
OpenGL renderer suite passes 36 checks; the previous renderer fails the new
incomplete-frame rejection check.

Local tests loaded Skyloft and an Eldin Volcano lava-fountain area using isolated
saves. A fast camera orbit was captured with the fountain visible. These are
bounded tests; the exact location reported by the player is unknown, and the
captures do not prove that every geyser or flashing issue is resolved.

On the test PC (RX 7900 XTX), five-second Skyloft measurement windows at 3x
resolution showed about 45–56 displayed fps with the final installed preview,
versus about 34–40 in an installed Alpha 2 baseline. A same-candidate test using
the old budget settings showed about 35–50. The final installed test kept real
game frames at about 30 fps with zero merged or dropped frames. Before the final
timing correction, a candidate run showed 48–58; a separate installed run
showed 36–50 and one merged frame. A 2x preview test showed about 46–57
displayed fps, so reducing resolution alone did not eliminate the dips.
These short local samples are not a steady-60 guarantee or a hardware benchmark.
Capture-enabled runs are unsuitable for performance comparisons because image
and memory capture adds overhead and changes the parsing path.

Game logic still runs at 30 Hz. Interpolation computes in-between images;
native 60/144-fps gameplay is not implemented. Holding ambiguous effects at
their current geometry may leave their own animation at the original cadence.
Bright lava, blur, camera clipping and other alpha rendering issues remain
under investigation.

Compare the affected scene with interpolation enabled and disabled. Include
the preview version, route, camera movement, graphics settings and enabled mods
in an F8 report. A short video helps show flashing and position jumps.
