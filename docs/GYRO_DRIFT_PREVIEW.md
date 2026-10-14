# 0.22.0 Preview 3 — Beetle steering drift

Preview 3 fixes two independent calibration paths that could treat slow,
intentional yaw as a sensor offset. This changed the neutral orientation during
Beetle flight, caused movement after stopping and carried the offset into aiming.

SkyBreak's sensor tracker and the original Wii MotionPlus drift detector now
freeze automatic bias learning while the game uses a pointing or steering
reference: item aiming, Beetle and Loftwing flight, swimming, tightrope balance,
free fall, wheels, carrying, motion puzzles and events. The decision uses the
raw game context, so a visible menu cursor cannot accidentally reenable learning
during flight. Ordinary menus and play can still collect stationary calibration.

A guided still calibration remains authoritative until the connected device
changes or another calibration is applied. It survives gyro mode toggles and
continues to apply when automatic calibration is disabled. MotionPlus preserves
its original drift setting outside each guarded read; input axes, gravity and
game timing are unchanged.

The production input regression reproduces slow turning, stopping and returning
to the launch orientation with 250/1000 Hz sensors, classic/Wii/hybrid presets
and cursor overlays. The source-only regression also covers other reference
controls, automatic calibration resuming at rest and guided calibration.
An additional local test feeds the original Wii MotionPlus code from an
independent rigid-body sensor model; its measured orientation tracks a
six-second turn instead of nearly erasing it.

Validation passes 30 source-only checks, 162 production input checks and 93
checks using original MotionPlus/game code. Before the bridge fix, a 0.15-rad
turn reached only 0.00630 rad; after it, the same turn reached 0.149815 rad,
with 0.000736 rad of movement over the three-second hold. A private-save test
also confirms the installed game loads successfully.

These are controlled regressions. Physical controller feel and longer gameplay
still need tester feedback. After installing, reconnect the controller, use
**Settings > Controls > Calibrate gyro**, keep it still and apply the result.
Point it comfortably toward the screen and tap the touchpad to recenter.

Preview 3 is a development build. The Alpha 2 download does not contain these
changes. Original game data, generated game code and saves remain local.
