# Online co-op: initial engine work

Target: two players who can move, fight, and interact in the same world, each on
their own PC. **Online co-op is not playable or included as a feature in Alpha 3.**

The first milestone is an independently controlled second player in one running
game. Networking alone cannot provide that capability.

## Findings from the current game

The SOUE01 game exposes `dAcPy_c::LINK` at `0x8057578C` and `LINK2` at `0x805757B8`.
During the tested Lanayru save both pointed to the same actor, `0x80B7652C`.
The model constructor writes `LINK2` and the player initialization writes `LINK`.
`LINK2` is also used by arrows and the Beetle. Treating it as a spare player slot
would redirect item ownership rather than establish a second independent player.

Input has four controller channels, but player helpers use a selected `dPad::ex_c`
and `mPad` Core. The camera and numerous items read the shared active player.
Changing channel 1 without isolating actor and item state is insufficient.

The opt-in `NWII_COOP_AUDIT=1` observation logs these pointers and the selected
input channel during gameplay. It does not write guest memory, saves, or enable
networking. The normal runtime leaves this observation disabled.

## Implemented foundation

`src/multiplayer/player_context.h` is an **inactive prototype**, not a gameplay hook.
It can temporarily switch the two player aliases and corresponding pad/Core/channel
as one scope. It saves the previous globals, restores nested contexts and exception
paths, and rolls back partial failures. Invalid pointers, missing controller Cores,
and invalid channels are rejected before any writes. A failed restoration must stop
guest execution; the destructor fails closed if restoration cannot complete.

The initial 27 checks passed. The prototype has not been used to execute a second
actor in the real game. It does not isolate inventory, enemies, HUD, events, item
ownership, save progress, or cameras. It does not create a second Link or implement
any network transport. Guest actor addresses must remain locally generated handles,
never addresses supplied by network packets.

## Next work toward a playable prototype

1. Create a second Link through the original actor factory in a private test save.
   Verify independent initialization, model resources, collision, deletion, and
   stage changes. Never clone a live actor by copying its memory.
2. Execute each player with its own input and player context. Identify remaining
   globals and assign spawned arrows, Beetles, swords, and interactions to owners.
3. Establish one host as authority for shared enemies, puzzle flags, scene changes,
   and progression. Each player needs an independent camera, health, and input.
4. Connect two matching game builds and synchronize players and world events.
   Test joining, lag, packet loss, disconnection, reconnects, and incompatible builds.
5. Validate one small area before expanding to combat, motion items, Timeshift
   Stones, shared puzzles, cutscenes, and saves. Leave real user saves isolated until
   co-op progress and recovery rules have been verified.

No completion date is established. A successful context test does not prove
gameplay compatibility or working online multiplayer.
