# 0.22.0 Preview 4 — button mapping

This development update fixes two independently reproduced mapping failures.

- An explicit HD layout could be silently replaced by the Wii motion layout
  when `gamepad_motion = "gyro"`. The editor displayed the HD binding table,
  but gameplay read the classic/Wii table. Layout selection now remains
  authoritative. Old settings that specify gyro without any layout still migrate
  to Wii motion when loaded.
- Captured bindings changed the editor outside its drawing pass, so the live
  preview missed them until Save. Captured bindings now reach the runtime
  immediately; Save persists them and discarding changes restores saved settings.

Releases swallowed during capture now reach the menu input system, preventing
previously pressed navigation keys/buttons from remaining held. F1, F8, F11 and
F12 remain reserved for application shortcuts. Capture rejects them rather than
creating a gameplay binding that cannot work; reserved keys do not activate
launcher shortcuts during capture. English capture feedback explains immediate
application and saving.

The production regression covers explicit HD remapping with gyro on, removal of
the previous action binding and legacy settings migration, alongside existing
controller, keyboard, gyro and context tests. A real launcher test reproduces
the old editor/runtime mismatch, then verifies capture, live application,
reserved keys, release handling, repeated edits, saving and restart persistence
with private settings. UI model and original Wii motion regressions are also run.

No tester's report has been treated as a confirmed diagnosis: controller type,
version and the specific failed assignment still help identify other cases.
This update is on the development branch; the existing Alpha 2 installer download
does not contain it. Game data and generated game code are not included in the
source-only mapping regression.
