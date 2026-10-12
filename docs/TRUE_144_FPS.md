# True 144 fps: current outcome

Not implemented. No frame-generation setting is presented as true 144 fps.
The experimental uniform 144-Hz interpolation work was removed from the release
source after the user explicitly selected real game frames only. It is excluded
from the community package and GitHub changes.

The existing Wii engine uses integer retrace counts: dSys_c::setFrameRate writes
EGG::Display::mMaxRetraces (1 = 60, 2 = 30). Display::preVRetrace advances the next
framebuffer when that count is reached. 144 cannot be represented by that
integer setting on a 60-Hz Wii retrace clock.

Changing the guest clock would affect simulation, not just presentation.
SaveMgr and dPad helpers derive some durations from 60/getFrameRate; other
durations have literal frame counts. Animation frame controllers have per-update
rates. Actors contain per-update movement/chase steps. Raising updates from 30
to 144 without changing these semantics gives 4.8 times as many updates per
second. That factor is a mathematical consequence, not a measured working mode.

The local decomp's d_a_player.cpp contains only constructor/destructor scaffolding
(216 bytes). Link's actual movement/physics still execute translated original
instructions, so their timing cannot be ported by editing that small source file.

Required validation for a future implementation: actual new gameplay/render
frames near 144/s, matching wall-clock walking and falling trajectories, attack
and damage windows, animation/sound timing, UI/message timers, cutscenes, boss-key
puzzles and save/reload. Count native frames separately from presented frames;
repeated or generated images do not satisfy this request. A correct engine timing
port remains work; the current community preview honestly retains 30-Hz gameplay.

Investigation references in the local ZeldaRET decomp checkout: src/d/d_sys.cpp:466,
src/egg/core/eggDisplay.cpp:preVRetrace, src/toBeSorted/save_manager.cpp:getFrameRate,
src/d/d_pad_manager.cpp, src/d/a/d_a_player.cpp. Existing measured 30-Hz gameplay
and computed 60-fps presentation evidence is recorded under outputs/fps23.
