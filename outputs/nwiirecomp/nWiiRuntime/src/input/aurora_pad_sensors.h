#pragma once
// Controller-only adaptation of Aurora's PADHasSensor, PADSetSensorEnabled and
// PADGetSensorData (lib/dolphin/pad/pad.cpp, commit
// 51797212b9283e91cc03c273854d63b2421d8490).
// Copyright (c) 2022 Luke Street. MIT; see THIRD_PARTY_AURORA_LICENSE.txt.
// SkyBreak retains SDL2: its sensor calls return 0 on success, unlike SDL3's
// bool result. Device/phone fallback and Aurora's renderer/router are not used.
// All calls run on SkyBreak's existing controller thread, with its selected pad.
#include <SDL.h>
#include <algorithm>

namespace nwii::runtime::input::aurora_sensors {
inline bool has(SDL_GameController* controller, SDL_SensorType sensor) {
    return controller != nullptr && SDL_GameControllerHasSensor(controller, sensor) == SDL_TRUE;
}
inline bool enable(SDL_GameController* controller, SDL_SensorType sensor, bool enabled) {
    if (!has(controller, sensor)) return false;
    return SDL_GameControllerSetSensorEnabled(controller, sensor, enabled ? SDL_TRUE : SDL_FALSE) == 0;
}
inline bool read(SDL_GameController* controller, SDL_SensorType sensor, float* data, int count) {
    if (!data || count <= 0) return false;
    if (has(controller, sensor) && SDL_GameControllerGetSensorData(controller, sensor, data, count) == 0)
        return true;
    std::fill(data, data + count, 0.0f); // A failed read cannot reuse an old turn rate.
    return false;
}
// Track only successful transitions. A temporary driver failure must be retried
// on the next poll; the former request mask incorrectly recorded it as success.
inline bool transition(SDL_GameController* controller, SDL_SensorType sensor, bool wanted, bool& enabled) {
    if (wanted == enabled) return true;
    if (!enable(controller, sensor, wanted)) return false;
    enabled = wanted;
    return true;
}
} // namespace nwii::runtime::input::aurora_sensors
