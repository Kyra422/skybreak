#include "input/input_manager.h"
#include "sources/boot_replay_source.h"
#include "virtual_motion.h"
#include "motion_gesture.h"
#include "gamepad_motion.h"
#include <iostream>
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace nwii::runtime::input {

InputManager& InputManager::get() {
    static InputManager instance;
    return instance;
}

InputManager::InputManager() {
    for (int i = 0; i < 4; ++i) {
        gc_pads[i] = {0, 0, 0, 0, 0, 0, 0, 0, 0, -1};
        wii_motes[i] = {0, 0.5f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, -1};
    }
    const char* replay = std::getenv("NWII_SS_INPUT_REPLAY");
    const bool title_replay = replay && std::strcmp(replay, "boot-title") == 0;
    if (title_replay || (replay && std::strcmp(replay, "boot") == 0)) {
        replay_source = std::make_unique<BootReplaySource>(title_replay);
        std::cout << "[InputManager] Explicit " << (title_replay ? "BOOT-TITLE" : "BOOT")
                  << " INPUT REPLAY enabled: virtual neutral motion, centered pointer, "
                  << (title_replay ? "400 ms A pulses for the first 120 seconds, then 1 second A pulses through 300 seconds"
                                   : "400 ms A pulses through 120 seconds")
                  << "; one pulse every 3 seconds; live sources disabled.\n";
    } else if (replay && std::strcmp(replay, "new-game") == 0) {
        replay_source = std::make_unique<BootReplaySource>(BootReplaySource::Mode::NewGame);
        std::cout << "[InputManager] Explicit NEW-GAME INPUT REPLAY enabled: BOOT-TITLE pulses until name "
                     "entry, then a scripted keyboard sequence (keys, OK); live sources disabled.\n";
    } else if (replay && std::strcmp(replay, "new-game-long") == 0) {
        // INT12 (PROG11): the literal name selects the progression director directly
        // (same as new-game + NWII_SS_REPLAY_LONG=1, which probe.py sets).
        replay_source = std::make_unique<BootReplaySource>(BootReplaySource::Mode::NewGameLong);
        std::cout << "[InputManager] Explicit NEW-GAME-LONG INPUT REPLAY enabled: the new-game replay, then the "
                     "progression director (out of the Knight Academy into F000); live sources disabled.\n";
    } else if (replay && std::strcmp(replay, "load-save") == 0) {
        // LOAD14 (M6.3): boot -> title -> file select -> the most recently saved used slot -> its prompts -> the
        // saved spot; NWII_SS_REPLAY_SAVE=1 then saves again at the nearest bird statue (save round trip).
        replay_source = std::make_unique<BootReplaySource>(BootReplaySource::Mode::LoadSave);
        std::cout << "[InputManager] Explicit LOAD-SAVE INPUT REPLAY enabled: BOOT-TITLE pulses until the file "
                     "select, then the most recently saved slot is pointed at and confirmed; live sources disabled.\n";
    }
}

void InputManager::add_source(std::unique_ptr<IInputSource> source) {
    if (source) {
        size_t count;
        {
            std::lock_guard lock(sources_mutex);
            sources.push_back(std::move(source));
            count = sources.size();
        }
        std::cout << "[InputManager] Registered a new input source. Total sources: " << count << std::endl;
    }
}

void InputManager::set_mode(InputMode mode) {
    {
        std::lock_guard lock(state_mutex);
        current_mode = mode;
    }
    std::cout << "[InputManager] Switched input mode to " << (int)mode << std::endl;
}

InputMode InputManager::get_mode() const {
    std::lock_guard lock(state_mutex);
    return current_mode;
}

void InputManager::update() {
    std::lock_guard update_lock(update_mutex);
    update_locked(motion_now_ns());
}

void InputManager::update_at(uint64_t monotonic_ns) {
    std::lock_guard update_lock(update_mutex);
    update_locked(monotonic_ns);
}

void InputManager::update_locked(uint64_t monotonic_ns) {
    // Serialize source polling, but do not hold a reader lock across external
    // source callbacks. A callback may query the previous frame or input mode.
    const uint64_t sequence = ++motion_sequence;
    const uint64_t elapsed = monotonic_ns > previous_update_ns ? monotonic_ns - previous_update_ns : 0;
    // First frame, a stalled producer, or a nonmonotonic timestamp establishes
    // a new baseline. Do not turn accumulated mouse deltas into a motion spike.
    const float dt = previous_update_ns != 0 && elapsed != 0 && elapsed <= kMotionMaxAgeNs
        ? static_cast<float>(elapsed) / 1'000'000'000.0f : 0.0f;
    previous_update_ns = monotonic_ns;
    {
        // SOAK14: the closed-loop aim (virtual_motion.h PointerAimLoop) learns the game's pointer offset from the
        // read-back (ss_boot.cpp) while the remote rests; read before the sources aim this update.
        const uint32_t reads = g_guest_pointer_reads.load(std::memory_order_acquire);
        const auto context = g_guest_context.get();
        if (!(context.valid && (context.prompt_slot[2] == 130 || context.prompt_slot[3] == 129)))
            g_pointer_aim[0].observe(g_virtual_orientation[0], dt, g_guest_pointer_x.load(), g_guest_pointer_y.load(), reads);
        // A puzzle's clamped UI pointer is not orientation feedback for a full 3-D rotation.
    }
    GameCubePadState next_gc_pads[4];
    WiimoteState next_wii_motes[4];
    {
        std::lock_guard lock(state_mutex);
        std::copy_n(gc_pads, 4, next_gc_pads);
        std::copy_n(wii_motes, 4, next_wii_motes);
    }
    std::vector<IInputSource*> frame_sources;
    {
        std::lock_guard lock(sources_mutex);
        frame_sources.reserve(sources.size());
        for (const auto& source : sources)
            frame_sources.push_back(source.get());
    }
    // Source objects remain owned until manager destruction. New sources are
    // included on the next update, even if registered by a source callback.
    for (int i = 0; i < 4; ++i) {
        next_gc_pads[i].buttons = 0;
        next_gc_pads[i].stick_x = 0;
        next_gc_pads[i].stick_y = 0;
        next_gc_pads[i].substick_x = 0;
        next_gc_pads[i].substick_y = 0;
        next_gc_pads[i].trigger_l = 0;
        next_gc_pads[i].trigger_r = 0;
        next_gc_pads[i].analog_a = 0;
        next_gc_pads[i].analog_b = 0;
        next_gc_pads[i].err = -1;

        next_wii_motes[i].buttons = 0;
        next_wii_motes[i].err = -1;
        next_wii_motes[i].nunchuk = {};
        next_wii_motes[i].motion = {};
        next_wii_motes[i].motion.sequence = sequence;
        next_wii_motes[i].motion.monotonic_ns = monotonic_ns;
        next_wii_motes[i].motion.dt_seconds = dt;
    }
    if (replay_source) {
        replay_source->update(next_gc_pads, next_wii_motes);
    } else {
        for (auto* source : frame_sources)
            source->update(next_gc_pads, next_wii_motes);
    }
    for (int i = 0; i < 4; ++i) {
        auto& mote = next_wii_motes[i];
        if (mote.motion.kind != MotionSourceKind::Virtual) continue;
        // Virtual remote model (virtual_motion.h): the published rates rotate
        // one orientation, and the accelerometer reports gravity for that
        // orientation (KPAD (0,-1,0) when level), so the original KMPLS
        // accelerometer revision agrees with the gyro instead of pulling the
        // remote back to a fixed pose. Every fresh producer step is recorded
        // for a guest that reads KPAD less often than this update runs.
        auto& orientation = g_virtual_orientation[i];
        // Requested gestures (motion_gesture.h: swings, thrust, Nunchuk flick/shake, raise) own
        // the remote's rates while active and add the sensor's linear acceleration; they are
        // stepped with the same producer interval, before the orientation integrates it.
        GestureStepOutput gesture;
        if (mote.motion.available) {
            g_virtual_gesture[i].latch_home(orientation);
            g_virtual_gesture[i].step(mote.motion.dt_seconds, orientation, gesture);
            if (gesture.remote_active) {
                mote.motion.gyro_x_rad_s = gesture.rate[0];
                mote.motion.gyro_y_rad_s = gesture.rate[1];
                mote.motion.gyro_z_rad_s = gesture.rate[2];
            }
        }
        // INPUT10: a controller's real gyro/accelerometer (gamepad_motion.h) drive this remote. Each
        // sensor sub-step becomes one recorded MotionStep of this update (same sequence), integrated in
        // order, with the measured acceleration; a requested gesture (keyboard) still takes over the
        // remote for the updates it is active.
        const MeasuredMotionFrame& measured = g_measured_motion[i];
        // Sensor capture and host polling have separate clocks. A 1 kHz host
        // must not add three fake zero-rate/accel intervals between 250 Hz
        // hardware reports. Keep motion identity/history unchanged meanwhile;
        // buttons and Nunchuk controls still update on every host poll.
        if (mote.motion.available && !gesture.remote_active &&
            g_gyro_drives_channel[i].load() && measured.sequence == mote.motion.sequence && measured.count == 0) {
            const WiimoteState previous = get_wiimote_state(i);
            mote.motion = previous.motion;
            mote.accel_x = previous.accel_x;
            mote.accel_y = previous.accel_y;
            mote.accel_z = previous.accel_z;
            continue;
        }
        const auto valid_sub = [](const MeasuredSubStep& sub) {
            return sub.dt > 0.0f && sub.dt <= 0.1f && std::isfinite(sub.rate[0]) && std::isfinite(sub.rate[1]) &&
                   std::isfinite(sub.rate[2]) && std::isfinite(sub.accel[0]) && std::isfinite(sub.accel[1]) &&
                   std::isfinite(sub.accel[2]);
        };
        bool measured_ok = mote.motion.available && !gesture.remote_active &&
                           measured.sequence == mote.motion.sequence && measured.count > 0 &&
                           measured.count <= kMaxMeasuredSubSteps;
        if (measured_ok) {
            measured_ok = false;
            for (size_t k = 0; k < measured.count; ++k) measured_ok = measured_ok || valid_sub(measured.steps[k]);
        }
        if (measured_ok) {
            mote.motion.sensor_measured = true;
            float nunchuk_accel[3] = {mote.nunchuk.accel_x, mote.nunchuk.accel_y, mote.nunchuk.accel_z};
            if (gesture.nunchuk_active && mote.nunchuk.available) {
                for (int k = 0; k < 3; ++k) nunchuk_accel[k] += gesture.nunchuk_accel[k];
                VirtualGestureController::clamp_accel(nunchuk_accel, kNunchukAccelRangeG);
                mote.nunchuk.accel_x = nunchuk_accel[0];
                mote.nunchuk.accel_y = nunchuk_accel[1];
                mote.nunchuk.accel_z = nunchuk_accel[2];
            }
            double angle[3] = {0, 0, 0}, total = 0;
            float last_accel[3] = {mote.accel_x, mote.accel_y, mote.accel_z};
            for (size_t k = 0; k < measured.count; ++k) {
                const MeasuredSubStep& sub = measured.steps[k];
                if (!valid_sub(sub)) continue;
                MotionStep step;
                step.sequence = mote.motion.sequence;
                step.monotonic_ns = mote.motion.monotonic_ns;
                step.dt_seconds = sub.dt;
                for (int a = 0; a < 3; ++a)
                    step.rate[a] = std::clamp(sub.rate[a], -kVirtualMotionMaxRateRadS, kVirtualMotionMaxRateRadS);
                float acc[3] = {sub.accel[0], sub.accel[1], sub.accel[2]};
                VirtualGestureController::clamp_accel(acc, kRemoteAccelRangeG);
                orientation.integrate(step.rate, step.dt_seconds);
                correct_tilt(orientation, acc, step.dt_seconds);
                for (int a = 0; a < 3; ++a) {
                    step.accel[a] = acc[a];
                    step.nunchuk_accel[a] = nunchuk_accel[a];
                    angle[a] += double(step.rate[a]) * step.dt_seconds;
                    last_accel[a] = acc[a];
                }
                total += step.dt_seconds;
                g_virtual_motion_history[i].push(step);
            }
            // The snapshot carries the update's mean rate (single-sample consumers) and the newest
            // accelerometer reading.
            mote.motion.gyro_x_rad_s = static_cast<float>(angle[0] / total);
            mote.motion.gyro_y_rad_s = static_cast<float>(angle[1] / total);
            mote.motion.gyro_z_rad_s = static_cast<float>(angle[2] / total);
            mote.accel_x = last_accel[0];
            mote.accel_y = last_accel[1];
            mote.accel_z = last_accel[2];
            continue;
        }
        MotionStep step;
        if (mote.motion.available) {
            step.sequence = mote.motion.sequence;
            step.monotonic_ns = mote.motion.monotonic_ns;
            step.dt_seconds = mote.motion.dt_seconds;
            step.rate[0] = mote.motion.gyro_x_rad_s;
            step.rate[1] = mote.motion.gyro_y_rad_s;
            step.rate[2] = mote.motion.gyro_z_rad_s;
            orientation.integrate(step.rate, step.dt_seconds);
        }
        float gravity[3];
        orientation.gravity(gravity);
        for (int k = 0; k < 3; ++k) gravity[k] += gesture.accel[k];
        VirtualGestureController::clamp_accel(gravity, kRemoteAccelRangeG);
        mote.accel_x = gravity[0];
        mote.accel_y = gravity[1];
        mote.accel_z = gravity[2];
        if (gesture.nunchuk_active && mote.nunchuk.available) {
            float n[3] = {mote.nunchuk.accel_x + gesture.nunchuk_accel[0],
                          mote.nunchuk.accel_y + gesture.nunchuk_accel[1],
                          mote.nunchuk.accel_z + gesture.nunchuk_accel[2]};
            VirtualGestureController::clamp_accel(n, kNunchukAccelRangeG);
            mote.nunchuk.accel_x = n[0];
            mote.nunchuk.accel_y = n[1];
            mote.nunchuk.accel_z = n[2];
        }
        if (mote.motion.available) {
            // The step also records this update's accelerometer readings (resample_accel_bins).
            step.accel[0] = mote.accel_x; step.accel[1] = mote.accel_y; step.accel[2] = mote.accel_z;
            step.nunchuk_accel[0] = mote.nunchuk.accel_x;
            step.nunchuk_accel[1] = mote.nunchuk.accel_y;
            step.nunchuk_accel[2] = mote.nunchuk.accel_z;
            g_virtual_motion_history[i].push(step);
        }
    }
    {
        std::lock_guard lock(state_mutex);
        std::copy_n(next_gc_pads, 4, gc_pads);
        std::copy_n(next_wii_motes, 4, wii_motes);
    }
}

GameCubePadState InputManager::get_gcpad_state(int index) {
    if (index >= 0 && index < 4) {
        std::lock_guard lock(state_mutex);
        return gc_pads[index];
    }
    return {0,0,0,0,0,0,0,0,0,-1};
}

WiimoteState InputManager::get_wiimote_state(int index) {
    if (index >= 0 && index < 4) {
        std::lock_guard lock(state_mutex);
        return wii_motes[index];
    }
    return {0,0.5f,0.5f,0,0,0,0,0,0,-1};
}

} 
