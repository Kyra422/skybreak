#pragma once
#include "input/input_source.h"
#include <cstdint>
#include <vector>
#include <memory>
#include <mutex>

namespace nwii::runtime::input {

enum class InputMode {
    GamepadClassic, 
    GamepadWiimote, 
    MouseKeyboard,  
    Touch,          
    Remote          
};

class InputManager {
public:
    static InputManager& get();

    void update();
    // A caller-supplied steady-clock timestamp also permits deterministic
    // producer tests. Ordinary host polling uses update().
    void update_at(uint64_t monotonic_ns);
    void set_mode(InputMode mode);
    InputMode get_mode() const;
    // INPUT13: false while an explicit replay (NWII_SS_INPUT_REPLAY) replaces the live sources; main.cpp
    // then starts no controller thread (a probe never opens the player's controller).
    bool live_sources_enabled() const { return !replay_source; }

    void add_source(std::unique_ptr<IInputSource> source);

    GameCubePadState get_gcpad_state(int index);
    WiimoteState get_wiimote_state(int index);

private:
    InputManager();
    ~InputManager() = default;
    void update_locked(uint64_t monotonic_ns);

    InputMode current_mode = InputMode::GamepadClassic;

    GameCubePadState gc_pads[4];
    WiimoteState wii_motes[4];

    std::vector<std::unique_ptr<IInputSource>> sources;
    std::unique_ptr<IInputSource> replay_source;

    // Sources build a frame privately. Readers only copy the last complete
    // frame, without waiting for source polling or observing partially merged
    // input. Separate locks let sources query the manager while being polled.
    mutable std::mutex state_mutex;
    std::mutex sources_mutex;
    std::mutex update_mutex;
    uint64_t motion_sequence = 0;
    uint64_t previous_update_ns = 0;
};

} 

