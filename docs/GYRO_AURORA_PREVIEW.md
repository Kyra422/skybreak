# Alpha 3 gyro changes

SkyBreak no longer learns gyro bias during gameplay. A slow turn can leave
gravity unchanged, so it cannot safely be treated as a stationary calibration.
The Wii MotionPlus bridge also clears its stored secondary drift offsets when
consuming SkyBreak's calibrated stream. Otherwise an old offset can keep
turning the pointer after the controller stops.

The controller path integrates a controller-only adaptation of Aurora's
`PADHasSensor`, `PADSetSensorEnabled` and `PADGetSensorData`, from
[`lib/dolphin/pad/pad.cpp`](https://github.com/encounter/aurora/blob/51797212b9283e91cc03c273854d63b2421d8490/lib/dolphin/pad/pad.cpp),
revision `51797212b9283e91cc03c273854d63b2421d8490`, under MIT. The adaptation
uses SkyBreak's selected controller and SDL2 success convention. It checks
sensor activation, retries failed transitions and clears failed reads.
Aurora's full SDL3 application, input router and graphics runtime are separate
from this adaptation and are not linked into this build.

Idle suppression now covers full Wii motion, steering and puzzles as well as
aiming. The default factory-only threshold is **0.02 rad/s**. Applying guided
calibration reduces it to at most **0.002 rad/s** for fine movement. The
configured threshold can be lowered, or set to zero to disable suppression.
Accelerometer data and three motion axes remain physical. Only aiming uses
the additional smoothing filter.

Only fresh gyro events advance rotation. Accelerometer-only packets cannot
keep replaying a stale turn. Tiny numerical tails from the original Wii filter
are delivered as zero below `1e-7` revolutions/s, well below an ADC count, to
avoid overflow in the original single-precision sword-vector calculation.

## Calibrate and test

1. Connect the controller and open **Settings > Controls > Calibrate gyro**.
2. Put it on a stable surface, choose **Measure stillness**, wait for the
   measurement to finish, then choose **Apply correction**.
3. Test the preview pointer, then try aiming and the Beetle in game. Tap the
   recenter binding in your current grip to choose the neutral direction.
4. Recalibrate after reconnecting or restarting, and if the controller's
   temperature changes enough to produce an offset.

Calibration and recentering have different purposes: calibration removes the
sensor offset, while recentering chooses the current pointing direction.

## Validation and limits

- The previous runtime passed 4 of 12 new drift-transition checks. The new
  runtime passes all 16, including a minute at rest, a normal-play-to-Beetle
  transition, opposite turns, lost gyro updates and calibration across toggles.
- Aurora adapter checks cover success, failure, retry and stale readings.
- Production input regression: 185 checks, including layouts, mappings,
  tutorial confirmation and Loftwing catch actions.
- Original Wii MotionPlus and game sword routines: 100 checks at 1000-Hz host /
  250-Hz sensor, and 100 checks at 30-Hz host / 1000-Hz sensor. This includes
  physical motion simulation with noise and bias, stopping, fine steering,
  sword directions, acceleration, recentering and reconnects.

The hardware probe found no connected sensor controller. These automated
checks do not establish how the final build feels on a physical DualSense,
nor validate every motion puzzle or a full playthrough. Report drift with F8,
including whether it continues with the controller resting on a table and
whether guided calibration was applied.
