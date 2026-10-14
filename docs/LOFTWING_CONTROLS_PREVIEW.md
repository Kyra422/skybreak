# 0.22.0 Preview 5 — Loftwing action and tutorial messages

An Alpha 2 tester using the Switch HD layout reported that the Xbox B prompt
did not let them advance the flight tutorial and that they could not grab the
ceremony bell. Preview 4 fixed an independent layout mismatch with gyro enabled.
This update fixes two additional, separately reproduced action-routing failures.

While flying, the HD action button always generated a wing-flap gesture. It now
sends the original Wii A button when the HUD requests Grab or Catch, without
also starting a flap. Keyboard action follows the same rule. Normal flight
keeps Xbox B for flapping, Xbox A for braking and Xbox Y for spiral charge.

The global event flag does not describe every message window. SkyBreak now reads
the actual SOUE01 message-window state, without modifying it. Active text,
confirmation and selection states use message controls even with an idle global
event manager. Invisible windows and passive Explain overlays keep gameplay
controls. A message opening also cancels queued synthetic gestures, preventing
a previous flap or swing from continuing through confirmation.

The expanded production-input regression verifies grab/catch, confirmation,
brake and flap separation, normal flight and keyboard actions. A source-only
message-state regression checks passive/modal states and rejects invalid
pointers or unrelated state names. No original game data/code is included in
these public tests.

These controlled tests establish the routing fixes. The tester's exact save and
settings have not been provided, and the entire Wing Ceremony has not been
replayed from their save. A full tutorial/bell retry on an updated build remains
necessary. The existing Alpha 2 installer download does not include Preview 5.
