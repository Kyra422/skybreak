#pragma once
// INPUT13: hand-over between the controller thread (the only code that calls SDL joystick/game controller
// functions) and the game-side gamepad source (main/input thread).
//
// Why: SDL_JoystickUpdate holds SDL's global joystick lock for the whole update, including device detection,
// and every SDL_GameController getter takes the same lock. In the 2026-10-03 play sessions detection hung for
// good inside a HID driver (dinput8 -> HidD_GetProductString, Bluetooth DualSense with Steam/Parsec/Virtual
// Desktop/G HUB virtual HID devices present) and took the window, GX and all input with it. Now the controller
// thread does SDL_JoystickUpdate + reads every controller into a snapshot; the gamepad source only reads the
// newest snapshot (lock-free triple buffer) and posts requests (sensors, rumble) as atomics. Keyboard and mouse
// never depend on this thread.
//
// SDL-free (tests include it without SDL).
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>

namespace nwii::runtime::input {

inline constexpr int kPadButtonCount = 21; // SDL_CONTROLLER_BUTTON_MAX (SDL 2.28)
inline constexpr int kPadAxisCount = 6;    // SDL_CONTROLLER_AXIS_MAX

enum class WiiIRStatus { Disconnected, Starting, Unavailable, Waiting, Stalled, NoBar, Visible };
inline WiiIRStatus wii_ir_status(bool supported,bool sensors,bool seen,bool live,bool valid) {
    if(!sensors) return WiiIRStatus::Starting;
    if(!supported) return WiiIRStatus::Unavailable;
    if(!seen) return WiiIRStatus::Waiting;
    if(!live) return WiiIRStatus::Stalled;
    return valid ? WiiIRStatus::Visible : WiiIRStatus::NoBar;
}
inline const char* wii_ir_status_text(WiiIRStatus status) {
    switch(status) {
    case WiiIRStatus::Starting: return "Sensor bar: starting camera";
    case WiiIRStatus::Unavailable: return "Sensor bar: IR input unavailable";
    case WiiIRStatus::Waiting: return "Sensor bar: waiting for camera reports";
    case WiiIRStatus::Stalled: return "Sensor bar: camera reports stopped";
    case WiiIRStatus::NoBar: return "Sensor bar: not visible (camera reports received)";
    case WiiIRStatus::Visible: return "Sensor bar: visible (physical IR)";
    default: return "Wii Remote disconnected";
    }
}

struct PadSnapshot {
    bool connected = false;
    int32_t instance_id = -1;   // SDL_JoystickID
    uint32_t generation = 0;    // changes whenever another device (or the same one again) fills this slot
    uint32_t buttons = 0;       // bit b = SDL_GameControllerButton b held
    int16_t axes[kPadAxisCount] = {};
    bool wii_remote = false, has_nunchuk = false;
    bool ir_supported=false, ir_valid=false;
    WiiIRStatus ir_status=WiiIRStatus::Disconnected;
    float ir_x=0.5f, ir_y=0.5f, ir_horizon_x=1.0f, ir_horizon_y=0.0f;
    bool has_gyro = false, has_accel = false;
    bool gyro_data_ok = false;  // legacy pointer modes 3/4: newest gyro value (SDL_GameControllerGetSensorData)
    float gyro_data[3] = {0, 0, 0};
    float gyro_rate_hz = 0.0f;  // SDL's declared sensor rate (DualSense: 1000 Bluetooth, 250 USB)
    int type = 0;               // SDL_GameControllerType
    char name[64] = {};
};

struct PadSnapshotSet {
    std::array<PadSnapshot, 4> pad{};
    uint64_t serial = 0; // publish counter
};

// Single-writer / single-reader triple buffer: the writer always fills a private buffer and swaps it into the
// middle; the reader swaps the middle into its own buffer when a newer one is there. No locks, no tearing.
template <typename T>
class TripleBuffer {
public:
    T& write_buffer() { return buf_[back_]; }
    void publish() { back_ = middle_.exchange(back_ | kDirty, std::memory_order_acq_rel) & kIndex; }
    const T& read() {
        if (middle_.load(std::memory_order_acquire) & kDirty)
            front_ = middle_.exchange(front_, std::memory_order_acq_rel) & kIndex;
        return buf_[front_];
    }

private:
    static constexpr uint32_t kIndex = 3, kDirty = 4;
    std::array<T, 3> buf_{};
    std::atomic<uint32_t> middle_{1};
    uint32_t back_ = 0, front_ = 2;
};

enum class PadBusy : int { None = 0, Init = 1, Update = 2, Capture = 3, WiiSensorSetup = 4 };

struct PadHub {
    TripleBuffer<PadSnapshotSet> snapshots;
    // true once the controller thread was started: from then on nothing but that thread calls SDL joystick
    // functions (the gamepad source never falls back to calling SDL itself, even while the thread is stuck).
    std::atomic<bool> threaded{false};
    std::atomic<bool> init_done{false}, init_failed{false};
    std::atomic<uint64_t> captures{0};
    // Watchdog: steady ms at which the current SDL call started (0 = not inside SDL) and which one.
    std::atomic<uint64_t> busy_since_ms{0};
    std::atomic<int> busy_what{0};
    // Requests from the gamepad source, per slot. Sensors: bit 0 gyro+accel (gyro motion), bit 1 gyro only
    // (legacy pointer modes 3/4, keeps reading the newest gyro value).
    std::array<std::atomic<int>, 4> want_sensors{};
    // Rumble: serial << 48 | duration_ms << 32 | low << 16 | high; a new serial = a new request.
    std::array<std::atomic<uint64_t>, 4> rumble{};
};

inline constexpr uint64_t kWiiSensorSetupTimeoutMs=4000;
inline uint64_t pad_input_timeout_ms(int what) {
    return what==int(PadBusy::WiiSensorSetup) ? kWiiSensorSetupTimeoutMs : 250;
}
inline bool pad_snapshot_stale(const PadHub& hub,uint64_t now) {
    const auto since=hub.busy_since_ms.load();
    return since && now>since && now-since>pad_input_timeout_ms(hub.busy_what.load());
}

inline PadHub& pad_hub() {
    static PadHub* hub = new PadHub; // never freed: the controller thread may outlive main() on a stuck driver
    return *hub;
}

inline uint64_t pad_now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

inline constexpr int kWiiRemotePadType = 100; // Host-only type; never passed to SDL.
inline std::atomic<bool> g_wii_remote_active[4];
inline std::atomic<bool> g_wii_sensorbar_visible[4];
inline std::atomic<WiiIRStatus> g_wii_ir_status[4];

// SDL_GameControllerType (SDL 2.28 numbering) -> short name for logs and the UI.
inline const char* pad_type_name(int type) {
    switch (type) {
    case kWiiRemotePadType: return "Wii Remote";
    case 1: case 2: return "Xbox";
    case 3: return "PS3";
    case 4: return "PS4";
    case 5: return "Switch Pro";
    case 6: return "Virtual";
    case 7: return "PS5";
    case 11: case 12: case 13: return "Joy-Con";
    default: return "Controller";
    }
}

} // namespace nwii::runtime::input
