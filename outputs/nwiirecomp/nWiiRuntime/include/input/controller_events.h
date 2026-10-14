#pragma once
// INPUT13: controller notifications for the UI (toasts) and a status query. SDL-free and thread-safe; none of
// these calls ever waits for a device driver (the controller thread owns every SDL joystick call).
//
// Usage (UI, once per frame):
//   nwii::runtime::input::ControllerEvent e;
//   while (nwii::runtime::input::poll_controller_event(e)) show_toast(e);
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>

namespace nwii::runtime::input {

enum class ControllerEventKind : uint8_t {
    Connected,          // a controller was opened (slot = Wii Remote index 0..3)
    Disconnected,       // it went away (Bluetooth sleep / cable / driver); its slot is free again
    DetectionBlocked,   // a device driver has not answered for a few seconds (keyboard + mouse keep working)
    DetectionRecovered, // the driver answered again
    GyroOn,             // gyro motion switched on (touchpad / Create+Options held 1 s, or the config)
    GyroOff,
    GyroRecentred,      // recentre: the current grip now aims at the screen centre
};

struct ControllerEvent {
    ControllerEventKind kind = ControllerEventKind::Connected;
    bool connected = false; // true for Connected, false for Disconnected (and for the other kinds)
    int slot = -1;          // Wii Remote index 0..3, -1 when not about one controller
    char name[64] = {};     // e.g. "DualSense Wireless Controller"
    char type[16] = {};     // "PS5", "PS4", "Xbox", "Switch Pro", "Joy-Con", "Controller"
    bool has_gyro = false;  // gyroscope + accelerometer available (gyro motion possible)
    uint64_t time_ms = 0;   // steady clock
};

struct ControllerStatus {
    bool connected = false;
    char name[64] = {};
    char type[16] = {};
    bool has_gyro = false;
    bool gyro_active = false;
};

namespace controller_events_detail {
struct Store {
    std::mutex mutex;
    std::deque<ControllerEvent> queue;
    ControllerStatus status[4];
    bool detection_blocked = false;
};
inline Store& store() {
    static Store* s = new Store; // never freed: may be used by the controller thread during shutdown
    return *s;
}
inline uint64_t now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
} // namespace controller_events_detail

// Producer side (input code). Keeps at most 32 pending events (oldest dropped).
inline void push_controller_event(ControllerEventKind kind, int slot, const char* name, const char* type,
                                  bool has_gyro) {
    ControllerEvent e;
    e.kind = kind;
    e.connected = kind == ControllerEventKind::Connected;
    e.slot = slot;
    if (name) std::snprintf(e.name, sizeof e.name, "%s", name);
    if (type) std::snprintf(e.type, sizeof e.type, "%s", type);
    e.has_gyro = has_gyro;
    e.time_ms = controller_events_detail::now_ms();
    auto& s = controller_events_detail::store();
    std::lock_guard lock(s.mutex);
    if (kind == ControllerEventKind::DetectionBlocked) s.detection_blocked = true;
    if (kind == ControllerEventKind::DetectionRecovered) s.detection_blocked = false;
    if (slot >= 0 && slot < 4) {
        auto& st = s.status[slot];
        if (kind == ControllerEventKind::Connected) {
            st = ControllerStatus{};
            st.connected = true;
            std::memcpy(st.name, e.name, sizeof st.name);
            std::memcpy(st.type, e.type, sizeof st.type);
            st.has_gyro = has_gyro;
        } else if (kind == ControllerEventKind::Disconnected) {
            st = ControllerStatus{};
        } else if (kind == ControllerEventKind::GyroOn) {
            st.gyro_active = true;
        } else if (kind == ControllerEventKind::GyroOff) {
            st.gyro_active = false;
        }
    }
    if (s.queue.size() >= 32) s.queue.pop_front();
    s.queue.push_back(e);
}

// Consumer side (UI). Returns false when no event is pending.
inline bool poll_controller_event(ControllerEvent& out) {
    auto& s = controller_events_detail::store();
    std::lock_guard lock(s.mutex);
    if (s.queue.empty()) return false;
    out = s.queue.front();
    s.queue.pop_front();
    return true;
}

// Current controllers per Wii Remote slot (for a settings/status panel). Returns the number connected.
inline int controller_status(ControllerStatus out[4], bool* detection_blocked = nullptr) {
    auto& s = controller_events_detail::store();
    std::lock_guard lock(s.mutex);
    int n = 0;
    for (int i = 0; i < 4; ++i) {
        out[i] = s.status[i];
        n += s.status[i].connected ? 1 : 0;
    }
    if (detection_blocked) *detection_blocked = s.detection_blocked;
    return n;
}

} // namespace nwii::runtime::input
