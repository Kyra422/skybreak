#include "gamepad_source.h"
#include "../game_observation.h"
#include "../virtual_motion.h"
#include "../input_bindings.h"
#include "../aurora_pad_sensors.h"
#include "../wii_remote_controls.h"
#include "input/controller_events.h"
#include "runtime/config.h"
#include <SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#pragma comment(lib, "user32.lib")
#endif

namespace nwii::runtime::input {
namespace {

// INPUT10: every SDL_CONTROLLERSENSORUPDATE (one gyro or accelerometer sample with its hardware
// timestamp) is captured by an event watch while SDL updates the joysticks (INPUT13: on the controller
// thread), so the gamepad source gets the whole sensor stream (250-1000 Hz), not only the newest value per
// host frame. Only joystick instances the source has claimed are recorded. Never freed: the watch can
// outlive the sources.
struct SensorCapture {
    std::mutex mutex;
    struct Slot {
        SDL_JoystickID id = -1;
        std::vector<SensorEvent> events;
        float nunchuk_accel[3]{};
        uint64_t nunchuk_ns = 0;
    };
    std::array<Slot, 8> slots;
};
SensorCapture& sensor_capture() {
    static SensorCapture* capture = new SensorCapture;
    return *capture;
}
int SDLCALL sensor_watch(void*, SDL_Event* event) {
    if (!event || event->type != SDL_CONTROLLERSENSORUPDATE) return 0;
    const SDL_ControllerSensorEvent& e = event->csensor;
    if (e.sensor != SDL_SENSOR_GYRO && e.sensor != SDL_SENSOR_ACCEL && e.sensor != SDL_SENSOR_ACCEL_L) return 0;
    // Wii HID reports have no hardware timestamp. Arrival time, rather than
    // the much faster host poll interval, defines their rotation duration.
    const uint64_t time_us = e.timestamp_us ? e.timestamp_us : motion_now_ns() / 1000;
    if (e.sensor != SDL_SENSOR_ACCEL_L)
        calibration_observe(e.which, e.sensor == SDL_SENSOR_GYRO, time_us, e.data);
    auto& capture = sensor_capture();
    std::lock_guard lock(capture.mutex);
    for (auto& slot : capture.slots) {
        if (slot.id != e.which) continue;
        if (e.sensor == SDL_SENSOR_ACCEL_L) {
            if (std::isfinite(e.data[0]) && std::isfinite(e.data[1]) && std::isfinite(e.data[2])) {
                std::copy(e.data, e.data + 3, slot.nunchuk_accel);
                slot.nunchuk_ns = motion_now_ns();
            }
            break;
        }
        if (slot.events.size() < 8192) {
            SensorEvent s;
            s.t_us = time_us;
            s.gyro = e.sensor == SDL_SENSOR_GYRO;
            for (int k = 0; k < 3; ++k) s.v[k] = e.data[k];
            slot.events.push_back(s);
        }
        break;
    }
    return 0;
}
void install_sensor_watch() {
    static const bool installed = (SDL_AddEventWatch(&sensor_watch, nullptr), true);
    (void)installed;
}
// Moves the events captured for `id` into `out` (oldest first); claims a slot on first use.
void drain_sensor_events(SDL_JoystickID id, std::vector<SensorEvent>& out) {
    out.clear();
    auto& capture = sensor_capture();
    std::lock_guard lock(capture.mutex);
    for (auto& slot : capture.slots)
        if (slot.id == id) { out.swap(slot.events); slot.events.clear(); return; }
    for (auto& slot : capture.slots)
        if (slot.id < 0) { slot.id = id; slot.events.clear(); return; }
}
void release_sensor_slot(SDL_JoystickID id) {
    auto& capture = sensor_capture();
    std::lock_guard lock(capture.mutex);
    for (auto& slot : capture.slots)
        if (slot.id == id) { slot.id = -1; slot.events.clear(); slot.nunchuk_ns = 0; }
}

bool read_nunchuk_accel(SDL_JoystickID id, float out[3]) {
    auto& capture = sensor_capture();
    std::lock_guard lock(capture.mutex);
    const auto now = motion_now_ns();
    for (const auto& slot : capture.slots)
        if (slot.id == id && slot.nunchuk_ns && now >= slot.nunchuk_ns && now - slot.nunchuk_ns <= kMotionMaxAgeNs) {
            controller_accel_to_kpad(slot.nunchuk_accel, out);
            return true;
        }
    return false;
}

// ---------------------------------------------------------------------------------------------------------
// INPUT13: controller capture. Runs on the controller thread (or inline when none was started): the only
// code that calls SDL joystick / game controller functions once the thread exists.
constexpr int kWantGyroAccel = 1; // gyro motion: gyroscope + accelerometer stream
constexpr int kWantGyroOnly = 2;  // legacy pointer modes 3/4: newest gyroscope value

struct CaptureDevice {
    SDL_GameController* controller = nullptr;
    SDL_JoystickID id = -1;
    uint32_t generation = 0;
    int applied_sensors = 0;
    bool gyro_enabled = false, accel_enabled = false, nunchuk_enabled = false;
    bool wii_remote = false, has_nunchuk = false;
    uint16_t ir_serial=0;
    uint64_t ir_ms=0;
    uint64_t rumble_serial = 0;
    int type = 0;
    float gyro_rate = 0.0f;
    // Used = a button pressed or a stick/trigger moved by 1/4 relative to its first reading (a virtual pad that
    // reports a constant odd value is not "used").
    bool used = false, have_rest = false;
    uint32_t rest_buttons = 0;
    int16_t rest_axes[kPadAxisCount] = {};
    char name[64] = {};
};
struct CaptureState {
    std::array<CaptureDevice, 4> slot{};
    uint32_t generations = 0;
    uint64_t serial = 0;
    struct Failed { SDL_JoystickID id; uint64_t retry_ms; };
    std::vector<Failed> failed;
    bool full_logged = false;
};
CaptureState& capture_state() {
    static CaptureState* state = new CaptureState;
    return *state;
}

void apply_requests(CaptureDevice& d, int slot) {
    auto& hub = pad_hub();
    const int want = hub.want_sensors[size_t(slot)].load() | (calibration_requested(slot) ? kWantGyroAccel : 0);
    if (want != d.applied_sensors) {
        const bool gyro_on = (want & (kWantGyroAccel | kWantGyroOnly)) != 0;
        const bool accel_on = (want & kWantGyroAccel) != 0;
        const bool gyro_ok = aurora_sensors::transition(d.controller, SDL_SENSOR_GYRO, gyro_on, d.gyro_enabled);
        const bool accel_ok = aurora_sensors::transition(d.controller, SDL_SENSOR_ACCEL, accel_on, d.accel_enabled);
        const bool nunchuk_ok = !d.has_nunchuk || aurora_sensors::transition(
            d.controller, SDL_SENSOR_ACCEL_L, accel_on, d.nunchuk_enabled);
        if (gyro_ok && accel_ok && nunchuk_ok) d.applied_sensors = want;
    }
    const uint64_t r = hub.rumble[size_t(slot)].load();
    if ((r >> 48) != d.rumble_serial) {
        d.rumble_serial = r >> 48;
        SDL_GameControllerRumble(d.controller, Uint16((r >> 16) & 0xFFFF), Uint16(r & 0xFFFF), Uint32((r >> 32) & 0xFFFF));
    }
}

void log_controller(const char* what, int slot, const CaptureDevice& d, bool has_gyro) {
    std::printf("[Input] Controller %s (slot %d = Wii Remote %d): %s, %s%s", what, slot, slot + 1, d.name,
                pad_type_name(d.type), has_gyro ? ", gyro + accelerometer" : "");
    if (has_gyro && d.gyro_rate > 0.0f) std::printf(" (SDL sensor rate %.0f Hz)", double(d.gyro_rate));
    std::printf("\n");
    std::fflush(stdout);
}

} // namespace

void capture_controllers(const ControllerApi* api) {
    install_sensor_watch();
    auto& st = capture_state();
    auto& hub = pad_hub();
    const uint64_t now = pad_now_ms();
    struct Present { int index; SDL_JoystickID id; };
    std::array<Present, 16> present{};
    int np = 0;
    const int n = api && api->num_joysticks ? std::min(api->num_joysticks(), 16) : 4;
    for (int idx = 0; idx < n; ++idx) {
        if (!SDL_IsGameController(idx)) continue;
        present[size_t(np++)] = {idx, SDL_JoystickGetDeviceInstanceID(idx)};
    }
    // Removed devices: close the handle (no stale handles; a Bluetooth reconnect is a new device).
    for (int s = 0; s < 4; ++s) {
        auto& d = st.slot[size_t(s)];
        if (!d.controller) continue;
        bool still = false;
        for (int k = 0; k < np; ++k) still = still || present[size_t(k)].id == d.id;
        if (still) continue;
        calibration_device(s, -1, false);
        log_controller("disconnected", s, d, false);
        push_controller_event(ControllerEventKind::Disconnected, s, d.name, pad_type_name(d.type), false);
        if (api && api->close) api->close(d.controller);
        d = CaptureDevice{};
    }
    // New devices: opened once, into the lowest free slot (slot 0 = Wii Remote 1, the one SS reads).
    for (int k = 0; k < np; ++k) {
        const SDL_JoystickID id = present[size_t(k)].id;
        bool known = false;
        for (const auto& d : st.slot) known = known || (d.controller && d.id == id);
        if (known) continue;
        bool skip = false;
        for (const auto& f : st.failed) skip = skip || (f.id == id && now < f.retry_ms);
        if (skip) continue;
        int slot = -1;
        for (int s = 0; s < 4 && slot < 0; ++s)
            if (!st.slot[size_t(s)].controller) slot = s;
        if (slot < 0) {
            if (!st.full_logged) {
                st.full_logged = true;
                std::printf("[Input] More than 4 controllers: the extra ones are ignored.\n");
                std::fflush(stdout);
            }
            continue;
        }
        SDL_GameController* c = SDL_GameControllerOpen(present[size_t(k)].index);
        if (!c) {
            bool logged = false;
            for (auto& f : st.failed)
                if (f.id == id) { f.retry_ms = now + 2000; logged = true; }
            if (!logged) {
                st.failed.push_back({id, now + 2000});
                std::printf("[Input] A controller could not be opened (%s); retrying every 2 s.\n",
                            api && api->error ? api->error() : "SDL_GameControllerOpen failed");
                std::fflush(stdout);
            }
            continue;
        }
        auto& d = st.slot[size_t(slot)];
        d = CaptureDevice{};
        d.controller = c;
        d.id = id;
        d.generation = ++st.generations;
        d.rumble_serial = hub.rumble[size_t(slot)].load() >> 48; // an older request is not replayed
        d.type = api && api->type ? api->type(c) : 0;
        d.gyro_rate = api && api->gyro_rate ? api->gyro_rate(c) : 0.0f;
        const char* name = SDL_GameControllerName(c);
        std::snprintf(d.name, sizeof d.name, "%s", name ? name : "controller");
        d.wii_remote = is_wii_remote_name(d.name);
        d.has_nunchuk = d.wii_remote && aurora_sensors::has(c, SDL_SENSOR_ACCEL_L);
        if (d.wii_remote) d.type = kWiiRemotePadType;
        const bool gyro = aurora_sensors::has(c, SDL_SENSOR_GYRO) && aurora_sensors::has(c, SDL_SENSOR_ACCEL);
        log_controller("connected", slot, d, gyro);
        push_controller_event(ControllerEventKind::Connected, slot, d.name, pad_type_name(d.type), gyro);
    }
    // A controller that is being used while Wii Remote 1 belongs to one that never was (a virtual pad of
    // Steam/Parsec/Virtual Desktop, or a forgotten second pad) becomes Wii Remote 1.
    PadSnapshotSet& out = hub.snapshots.write_buffer();
    for (int s = 0; s < 4; ++s) {
        PadSnapshot& p = out.pad[size_t(s)];
        p = PadSnapshot{};
        auto& d = st.slot[size_t(s)];
        if (!d.controller) continue;
        calibration_device(s, d.id, aurora_sensors::has(d.controller, SDL_SENSOR_GYRO) && aurora_sensors::has(d.controller, SDL_SENSOR_ACCEL));
        apply_requests(d, s);
        p.connected = true;
        p.instance_id = d.id;
        p.generation = d.generation;
        p.type = d.type;
        p.wii_remote = d.wii_remote;
        p.has_nunchuk = d.has_nunchuk;
        if(d.wii_remote && api && api->wii_ir) {
            float data[4];bool valid=false;uint16_t serial=0;
            p.ir_supported=api->wii_ir(d.controller,data,valid,serial);
            if(p.ir_supported) {
                if(serial!=d.ir_serial) {d.ir_serial=serial;d.ir_ms=now;}
                p.ir_valid=valid && d.ir_ms && now>=d.ir_ms && now-d.ir_ms<=100;
                p.ir_x=data[0];p.ir_y=data[1];p.ir_horizon_x=data[2];p.ir_horizon_y=data[3];
            }
        }
        p.gyro_rate_hz = d.gyro_rate;
        std::memcpy(p.name, d.name, sizeof p.name);
        for (int b = 0; b < kPadButtonCount; ++b)
            if (SDL_GameControllerGetButton(d.controller, SDL_GameControllerButton(b))) p.buttons |= 1u << b;
        for (int a = 0; a < kPadAxisCount; ++a)
            p.axes[a] = SDL_GameControllerGetAxis(d.controller, SDL_GameControllerAxis(a));
        if (!d.have_rest) {
            d.have_rest = true;
            d.rest_buttons = p.buttons;
            std::memcpy(d.rest_axes, p.axes, sizeof d.rest_axes);
        }
        d.used = d.used || (p.buttons & ~d.rest_buttons) != 0;
        for (int a = 0; a < kPadAxisCount; ++a) d.used = d.used || std::abs(int(p.axes[a]) - int(d.rest_axes[a])) > 8192;
        p.has_gyro = aurora_sensors::has(d.controller, SDL_SENSOR_GYRO);
        p.has_accel = aurora_sensors::has(d.controller, SDL_SENSOR_ACCEL);
        if ((d.applied_sensors & kWantGyroOnly) && p.has_gyro)
            p.gyro_data_ok = aurora_sensors::read(d.controller, SDL_SENSOR_GYRO, p.gyro_data, 3);
    }
    if (st.slot[0].controller && !st.slot[0].used) {
        for (int s = 1; s < 4; ++s) {
            auto& d = st.slot[size_t(s)];
            if (!d.controller || !d.used) continue;
            std::printf("[Input] %s is in use and becomes Wii Remote 1 (%s was idle).\n", d.name, st.slot[0].name);
            std::fflush(stdout);
            std::swap(st.slot[0], d);
            std::swap(out.pad[0], out.pad[size_t(s)]);
            // Both slots now hold another device: new generations, requests re-applied next time.
            for (int t : {0, s}) {
                auto& moved = st.slot[size_t(t)];
                moved.generation = ++st.generations;
                out.pad[size_t(t)].generation = moved.generation;
                moved.rumble_serial = hub.rumble[size_t(t)].load() >> 48;
            }
            push_controller_event(ControllerEventKind::Connected, 0, st.slot[0].name, pad_type_name(st.slot[0].type),
                                  out.pad[0].has_gyro && out.pad[0].has_accel);
            break;
        }
    }
    out.serial = ++st.serial;
    hub.snapshots.publish();
    hub.captures.fetch_add(1);
}

namespace {

// Requests to the capture: atomics for the controller thread; applied at once when capturing inline (tests).
void request_sensors(int slot, int want) {
    auto& hub = pad_hub();
    if (hub.want_sensors[size_t(slot)].exchange(want) == want || hub.threaded.load()) return;
    auto& d = capture_state().slot[size_t(slot)];
    if (d.controller) apply_requests(d, slot);
}
void request_rumble(int slot, uint16_t low, uint16_t high, uint16_t ms) {
    auto& hub = pad_hub();
    const uint64_t old = hub.rumble[size_t(slot)].load();
    uint64_t serial = ((old >> 48) + 1) & 0xFFFF;
    if (serial == 0) serial = 1;
    hub.rumble[size_t(slot)].store(serial << 48 | uint64_t(ms) << 32 | uint64_t(low) << 16 | high);
    if (hub.threaded.load()) return;
    auto& d = capture_state().slot[size_t(slot)];
    if (d.controller) apply_requests(d, slot);
}

// Gamepad bindings (input_bindings.h) evaluated on one controller snapshot. Buttons of a held multi-button
// gyro_toggle combo (default Create+Options) are `suppressed`: the game never sees them while held.
struct PadState {
    const PadSnapshot* snap = nullptr;
    int trigger_threshold = 16384;
    uint32_t suppressed = 0; // bit = SDL_GameControllerButton
};
bool pad_button(const PadState& p, SDL_GameControllerButton b) {
    if (b < 0 || b >= kPadButtonCount) return false;
    if (p.suppressed & (1u << b)) return false;
    return (p.snap->buttons >> b) & 1u;
}
int pad_axis(const PadState& p, SDL_GameControllerAxis a) {
    return a >= 0 && a < kPadAxisCount ? p.snap->axes[a] : 0;
}
bool pad_input_held(const PadState& p, const BoundInput& in) {
    if (in.kind == BoundInput::Button) return pad_button(p, SDL_GameControllerButton(in.code));
    // CTRL18-INPUT: face button by position (input_bindings.h face_button_for).
    if (in.kind == BoundInput::Face) return pad_button(p, SDL_GameControllerButton(face_button_for(in.code, p.snap->type)));
    if (in.kind == BoundInput::Axis) return pad_axis(p, SDL_GameControllerAxis(in.code)) > p.trigger_threshold;
    return false;
}
bool pad_combo_held(const PadState& p, const InputCombo& c) {
    if (c.n <= 0) return false;
    for (int k = 0; k < c.n; ++k)
        if (!pad_input_held(p, c.in[size_t(k)])) return false;
    return true;
}
const InputCombo* pad_action_combo(const PadState& p, PadAction a) {
    for (const auto& c : input_bindings().gamepad[size_t(a)])
        if (pad_combo_held(p, c)) return &c;
    return nullptr;
}
bool pad_action_held(const PadState& p, PadAction a) { return pad_action_combo(p, a) != nullptr; }
// CTRL18-INPUT: the switch-hd intent table ([input.gamepad_hd]).
const InputCombo* hd_action_combo(const PadState& p, HdAction a) {
    for (const auto& c : input_bindings().gamepad_hd[size_t(a)])
        if (pad_combo_held(p, c)) return &c;
    return nullptr;
}

// SS's pointer position (x right, y down, -1..1) for the remote's current forward direction, assuming the
// game's centre reference is the identity orientation (inverse of virtual_motion.h pointer_direction).
bool pointer_position(const VirtualRemoteOrientation& o, float& px, float& py) {
    double m[3][3];
    o.matrix(m);
    const double vx = m[0][2], vy = m[1][2], vz = m[2][2];
    if (!(vz > 0.0)) return false;
    px = static_cast<float>(std::atan2(-vx, vz) / kPointerHalfRangeRad);
    py = static_cast<float>(std::atan2(-vy, vz) / kPointerHalfRangeRad);
    return std::isfinite(px) && std::isfinite(py);
}

// CTRL18-INPUT: the controller is being used (a button, a stick past its deadzone or a trigger): it drives its Wii
// Remote (camera_stick.h g_active_device, rumble routing).
bool pad_in_use(const PadSnapshot& s, const InputBindings& b) {
    if (s.buttons) return true;
    return std::abs(int(s.axes[SDL_CONTROLLER_AXIS_LEFTX])) > b.left_deadzone ||
           std::abs(int(s.axes[SDL_CONTROLLER_AXIS_LEFTY])) > b.left_deadzone ||
           std::abs(int(s.axes[SDL_CONTROLLER_AXIS_RIGHTX])) > b.right_deadzone ||
           std::abs(int(s.axes[SDL_CONTROLLER_AXIS_RIGHTY])) > b.right_deadzone ||
           s.axes[SDL_CONTROLLER_AXIS_TRIGGERLEFT] > b.trigger_threshold ||
           s.axes[SDL_CONTROLLER_AXIS_TRIGGERRIGHT] > b.trigger_threshold;
}

// Deliberate controller rotation (rad/s, after the bias) that takes the pointer back from the mouse in gyro
// mode; a hand holding the pad still stays far below it.
constexpr float kGyroClaimRadS = 0.12f;

} // namespace

void GamepadSource::check_controller_thread() {
    auto& hub = pad_hub();
    if (!hub.threaded.load()) return;
    static const uint64_t limit_ms = [] {
        const char* e = std::getenv("NWII_PAD_BLOCKED_SECONDS"); // test hook; default 3 s
        const double s = e ? std::atof(e) : 3.0;
        return uint64_t((s > 0.05 && s < 600 ? s : 3.0) * 1000.0);
    }();
    const uint64_t since = hub.busy_since_ms.load();
    const uint64_t now = pad_now_ms();
    if (since != 0 && now > since && now - since >= limit_ms) {
        if (!blocked_reported) {
            blocked_reported = true;
            blocked_since_ms = since;
            const int what = hub.busy_what.load();
            std::printf("[Input] controller detection is blocked by a device driver: %s has not returned for %.0f s "
                        "(a Bluetooth or virtual HID device is not answering). Keyboard and mouse keep working; "
                        "controllers continue as soon as the driver answers. If this keeps happening: reconnect the "
                        "controller, or close Steam / Parsec / Virtual Desktop / G HUB.\n",
                        what == int(PadBusy::Init) ? "the controller start-up scan (SDL game controller init)"
                        : what == int(PadBusy::Update) ? "SDL_JoystickUpdate" : "reading the controllers",
                        double(now - since) / 1000.0);
            std::fflush(stdout);
            push_controller_event(ControllerEventKind::DetectionBlocked, -1, nullptr, nullptr, false);
        }
    } else if (blocked_reported && since != blocked_since_ms) {
        blocked_reported = false;
        std::printf("[Input] controller detection recovered (the driver answered after %.1f s).\n",
                    double(now - blocked_since_ms) / 1000.0);
        std::fflush(stdout);
        push_controller_event(ControllerEventKind::DetectionRecovered, -1, nullptr, nullptr, false);
    }
}

void GamepadSource::update(GameCubePadState pads[4], WiimoteState motes[4]) {
    auto& cfg = nwii::runtime::Config::get();
    apply_pending_input_bindings_reload(); // CTRL18-INPUT: preset swap / rebind from the UI, on this thread
    const InputBindings& bind = input_bindings();
    if (last_preset != int(bind.pad_preset)) {
        // CTRL18-INPUT: a (new) preset: its own gyro default; no translator or gesture state carries over.
        if (last_preset >= 0) {
            g_gamepad_motion_mode.store(bind.gyro_at_start ? 1 : 0);
            for (auto& t : hd) t.reset();
            std::printf("[Input] Controller preset \"%s\"%s\n", pad_preset_name(bind.pad_preset),
                        bind.gyro_at_start ? " (gyro on)" : "");
            std::fflush(stdout);
        }
        last_preset = int(bind.pad_preset);
    }
    // CTRL18-INPUT -> CTRL18-CAM: no free camera while a controller in the wii-motion preset drives Wii Remote 1.
    g_free_camera.scheme_allows.store(!(bind.pad_preset == PadPreset::WiiMotion &&
                                        g_active_device[0].load() == int(ActiveDevice::Gamepad)));
    int mode = cfg.input_mode;
    bool want_pointer = (mode == 3 || mode == 4);
    if (g_gamepad_motion_mode.load() < 0) {
        g_gamepad_motion_mode.store(bind.gyro_at_start ? 1 : 0);
        if (bind.gyro_at_start)
            std::printf("[Input] Gyro motion ON from the config ([input] gamepad_motion = \"gyro\"): a controller with "
                        "gyro + accelerometer is the Wii Remote; hold the touchpad or Create+Options 1 s to switch.\n");
    }

    // INPUT13: the controller thread publishes snapshots; without it (tests, headless) capture here.
    auto& hub = pad_hub();
    if (hub.threaded.load()) check_controller_thread();
    else capture_controllers(nullptr);
    const PadSnapshotSet* published = &hub.snapshots.read();
    if (hub.threaded.load()) {
        // The controller thread is stuck inside SDL (a driver call): its last snapshot would keep a held stick or
        // button held for as long as the driver hangs, so the controllers read as neutral meanwhile.
        static const PadSnapshotSet kNoPads{};
        const uint64_t since = hub.busy_since_ms.load();
        if (since != 0 && pad_now_ms() > since + 250) published = &kNoPads;
    }
    const PadSnapshotSet& snapshot = *published;

    const bool gyro_owned_before = g_aim_owner.load() == 1;
    bool any_gyro = false;

    for (int i = 0; i < 4; ++i) {
        const PadSnapshot& snap = snapshot.pad[size_t(i)];
        g_wii_remote_active[i].store(snap.connected && snap.wii_remote);
        g_wii_sensorbar_visible[i].store(snap.connected && snap.wii_remote && snap.ir_valid);
        if (snap.generation != generation[i]) {
            // Another device in this slot (or none): no gesture/toggle/gyro state carries over. sensor_id stays,
            // so a different device clears the gyro bias estimate on activation (INPUT10).
            generation[i] = snap.generation;
            if (sensors_on[i]) {
                release_sensor_slot(sensor_id[i]);
                gyro[i].discard();
                sensors_on[i] = false;
            }
            wii_nunchuk_seen[i] = false;
            wii_ir_reference_set[i] = false;
            flick[i] = StickFlick{};
            previous_rb[i] = previous_rt[i] = false;
            toggle_held_s[i] = 0.0;
            toggle_fired[i] = false;
            no_sensor_logged[i] = false;
            recenter_left_s[i] = 0.0;
            recenter_prev[i] = false;
            menu_prev[i] = false;
            hd[i].reset(); // CTRL18-INPUT
            hd_claimed[i] = hd_recentre_prev[i] = false;
        }
        if (!snap.connected) {
            g_gyro_drives_channel[i].store(false);
            request_sensors(i, 0);
            continue;
        }

        pads[i].err = 0;
        motes[i].err = 0;
        const float dt = motes[i].motion.dt_seconds;
        const bool dt_ok = dt > 0.0f && dt <= 0.1f;
        if (snap.wii_remote) {
            update_wii_remote(i, snap, motes[i], any_gyro);
            continue; // A real remote always uses its original layout, in every saved preset.
        }
        PadState pad;
        pad.snap = &snap;
        pad.trigger_threshold = bind.trigger_threshold;
        // CTRL18-INPUT: the switch-hd preset has its own path (hd_translator.h); classic and wii-motion continue here.
        if (bind.pad_preset == PadPreset::SwitchHd) {
            update_switch_hd(i, snap, pads[i], motes[i], any_gyro);
            continue;
        }

        // INPUT10: gyro_toggle held for 1 s switches gyro motion for all controllers. While a multi-button
        // combo of it (Create+Options) is held, those buttons are not passed on (no Wii + / -).
        if (const InputCombo* toggle = pad_action_combo(pad, kPadGyroToggle)) {
            if (toggle->n >= 2)
                for (int k = 0; k < toggle->n; ++k)
                    if (toggle->in[size_t(k)].kind == BoundInput::Button && toggle->in[size_t(k)].code < 32)
                        pad.suppressed |= 1u << toggle->in[size_t(k)].code;
            if (dt_ok) toggle_held_s[i] += dt;
            if (!toggle_fired[i] && toggle_held_s[i] >= 1.0) {
                toggle_fired[i] = true;
                const bool on = g_gamepad_motion_mode.load() != 1;
                g_gamepad_motion_mode.store(on ? 1 : 0);
                std::printf("[Input] Gyro motion %s (gyro_toggle held 1 s on controller %d, %s)\n", on ? "ON" : "OFF", i,
                            snap.name);
                std::fflush(stdout);
                request_rumble(i, on ? 0x7000 : 0x3000, on ? 0x7000 : 0x3000, on ? 200 : 100);
            }
        } else {
            toggle_held_s[i] = 0.0;
            toggle_fired[i] = false;
        }

        // INPUT10 gyro mode: the controller's own gyroscope + accelerometer are the remote's sensors.
        const bool gyro_mode = g_gamepad_motion_mode.load() == 1;
        const bool has_sensors = gyro_mode && snap.has_gyro && snap.has_accel;
        const bool recenter_held = pad_action_held(pad, kPadRecenter);
        bool gyro_active = false;
        if (gyro_mode && has_sensors) {
            const SDL_JoystickID id = snap.instance_id;
            if (!sensors_on[i] || id != sensor_id[i]) {
                install_sensor_watch();
                if (sensor_id[i] != id) {
                    if (sensor_id[i] >= 0) release_sensor_slot(sensor_id[i]);
                    gyro[i].reset_bias();
                calibration_serial[i]=0;gravity_aim[i].reset(); // another device
                }
                request_sensors(i, kWantGyroAccel);
                drain_sensor_events(id, scratch); // claims the capture slot; no stale backlog
                gyro[i].reset();
                gyro[i].deadband = bind.gyro_deadband;
                gyro[i].grip_tilt = double(bind.gyro_grip_tilt_deg) * 3.14159265358979323846 / 180.0;
                sensor_id[i] = id;
                sensors_on[i] = true;
                gyro_active_s[i] = 0.0;
                gyro_claim_s[i] = 0.0;
                stats_logged[i] = false;
                recenter_prev[i] = recenter_held; // the touchpad that switched gyro on is no recentre
                recenter_left_s[i] = 0.0;
                g_aim_owner.store(1); // INPUT13: switching gyro on hands it the pointer
                std::printf("[Input] Gyro motion active on controller %d (%s): held face up, far edge to the screen = "
                            "Wii Remote aimed at the screen; tilt = pitch, turn = yaw, swing = sword. Tap the touchpad "
                            "(or L3+R3) to recentre: the way you hold it then = aimed at the screen centre.\n",
                            i, snap.name);
                std::fflush(stdout);
                push_controller_event(ControllerEventKind::GyroOn, i, snap.name, pad_type_name(snap.type), true);
            }
            drain_sensor_events(id, scratch);
            float manual_bias[3];if(calibration_bias(id,calibration_serial[i],manual_bias))gyro[i].set_bias(manual_bias);
            for (const auto& e : scratch) gyro[i].add(e);
            gyro_active = true;
        } else {
            if (sensors_on[i]) {
                request_sensors(i, 0);
                release_sensor_slot(sensor_id[i]); // the event watch stops recording this controller
                gyro[i].discard();
                sensors_on[i] = false;
                recenter_left_s[i] = 0.0;
                std::printf("[Input] Gyro motion off on controller %d: the right stick aims again.\n", i);
                std::fflush(stdout);
                push_controller_event(ControllerEventKind::GyroOff, i, snap.name, pad_type_name(snap.type),
                                      snap.has_gyro && snap.has_accel);
            }
            if (gyro_mode && !has_sensors && !no_sensor_logged[i]) {
                no_sensor_logged[i] = true;
                std::printf("[Input] Gyro motion requested, but controller %d (%s) reports no gyroscope + accelerometer; "
                            "using the sticks.\n", i, snap.name);
                std::fflush(stdout);
            }
        }
        g_gyro_drives_channel[i].store(gyro_active);
        any_gyro = any_gyro || gyro_active;

        static const SDL_GameControllerButton kClaim[] = {
            SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_Y,
            SDL_CONTROLLER_BUTTON_START, SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
            SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT, SDL_CONTROLLER_BUTTON_TOUCHPAD};
        bool button_claim = false;
        for (auto b : kClaim) button_claim = button_claim || pad_button(pad, b);

        float pitch = 0.0f, yaw = 0.0f, roll = 0.0f;
        if (gyro_active) {
            auto& frame = g_measured_motion[i];
            frame.sequence = 0;
            frame.count = 0;
            const bool point_center = g_guest_point_center.load();
            const GameContext ctx = g_guest_context.get();
            const Situation situation = classify(ctx, LiveFlags::read(i));
            gyro[i].filter_aim = situation == Situation::Aim || situation == Situation::Menu || situation == Situation::Event;
            gyro[i].deadband = bind.gyro_deadband;
            if (dt_ok) {
                const size_t n = gyro[i].produce(dt, frame.steps.data(), frame.steps.size());
                float peak = 0.0f;
                double sensor_dt = 0;
                for (size_t k = 0; k < n; ++k) {
                    sensor_dt += frame.steps[k].dt;
                    const float w = std::min(1.0f, frame.steps[k].dt / 0.2f);
                    recent_pitch[i] += (std::abs(frame.steps[k].rate[0]) - recent_pitch[i]) * w;
                    recent_yaw[i] += (std::abs(frame.steps[k].rate[1]) - recent_yaw[i]) * w;
                    peak = std::max(peak, std::sqrt(frame.steps[k].rate[0] * frame.steps[k].rate[0] +
                                                    frame.steps[k].rate[1] * frame.steps[k].rate[1] +
                                                    frame.steps[k].rate[2] * frame.steps[k].rate[2]));
                }
                gyro_active_s[i] += dt;
                if (!stats_logged[i] && gyro_active_s[i] >= 3.0) {
                    stats_logged[i] = true;
                    const float* b = gyro[i].bias();
                    std::printf("[Input] Gyro controller %d: %.0f samples/s, gyro bias %s (%.4f, %.4f, %.4f) rad/s\n", i,
                                double(gyro[i].samples_received) / gyro_active_s[i],
                                gyro[i].bias_valid() ? "guided calibration" : "factory calibration; use Controls > Gyro calibration if drifting",
                                double(b[0]), double(b[1]), double(b[2]));
                    std::fflush(stdout);
                }
                // INPUT13 pointer recovery. (1) Deliberate pad motion or a button takes the pointer back from the
                // mouse. (2) Recentre (touchpad tap / L3+R3): the current grip becomes "level, aimed at the screen"
                // (grip tilt from gravity) and the remote is steered to the screen centre. (3) The game's
                // point-at-screen step holds the remote at the centre and the grip in which it ends is captured.
                if (!scratch.empty()) gyro_claim_s[i] = peak > kGyroClaimRadS ? gyro_claim_s[i]+sensor_dt : 0;
                if ((!scratch.empty() && (peak > 0.8f || gyro_claim_s[i] >= 0.035)) || button_claim) g_aim_owner.store(1);
                double grip = 0.0;
                if (recenter_held && !recenter_prev[i]) {
                    if (gyro[i].grip_from_gravity(grip)) gyro[i].grip_tilt = grip;
                    recenter_left_s[i] = 0.6;
                    g_aim_owner.store(1);
                    std::printf("[Input] Gyro recentred on controller %d: this grip (far edge %+.0f deg) now aims at the "
                                "screen centre.\n", i, gyro[i].grip_tilt * 180.0 / 3.14159265358979323846);
                    std::fflush(stdout);
                    push_controller_event(ControllerEventKind::GyroRecentred, i, snap.name, pad_type_name(snap.type), true);
                }
                if (point_center_prev[i] && !point_center && gyro[i].grip_from_gravity(grip)) {
                    gyro[i].grip_tilt = grip;
                    std::printf("[Input] Gyro: the grip at the point-at-screen step (far edge %+.0f deg) aims at the "
                                "screen centre.\n", grip * 180.0 / 3.14159265358979323846);
                    std::fflush(stdout);
                }
                if (point_center || recenter_left_s[i] > 0.0) {
                    // Steer with virtual rates (the InputManager then reports the matching gravity); the measured
                    // frame is dropped meanwhile.
                    double target[3];
                    pointer_direction(0.0, 0.0, target);
                    float rates[3];
                    g_virtual_orientation[i].rates_toward(target, dt, kPointerMotionMaxRateRadS, rates);
                    pitch = rates[0];
                    yaw = rates[1];
                    roll = rates[2]; // SOAK14: level the remote too (virtual_motion.h level_roll_rate)
                    recenter_left_s[i] -= dt;
                    float px = 0, py = 0;
                    if (recenter_left_s[i] > 0.0 && pointer_position(g_virtual_orientation[i], px, py) &&
                        std::abs(px) < 0.01f && std::abs(py) < 0.01f)
                        recenter_left_s[i] = 0.0;
                } else if (g_aim_owner.load() == 1) {
                    frame.sequence = motes[i].motion.sequence;
                    frame.count = n;
                }
                // else: the mouse aims (it moved after the gyro did); the samples are consumed, not used.
            } else {
                gyro[i].discard();
            }
            point_center_prev[i] = point_center;
            recenter_prev[i] = recenter_held;
            // The real controller swings, thrusts and raises the remote; buttons add the Nunchuk part:
            // nunchuk_flick (RT) = shield, nunchuk_shake held (RB) = Nunchuk shake for a spin attack.
            if (g_guest_player_present.load()) {
                auto& gesture = g_virtual_gesture[i];
                const bool nflick = pad_action_held(pad, kPadNunchukFlick);
                if (nflick && !previous_rt[i]) gesture.nunchuk_flick();
                previous_rt[i] = nflick;
                if (pad_action_held(pad, kPadNunchukShake) && !gesture.nunchuk_active())
                    gesture.nunchuk_shake(recent_pitch[i] > recent_yaw[i]);
            }
            previous_rb[i] = pad_action_held(pad, kPadThrust); // no stale edge when switching back
            cursor_x[i] = cursor_y[i] = 0.0f;
            menu_prev[i] = false;

        } else {
            recenter_prev[i] = recenter_held;
            point_center_prev[i] = false;
            // Right stick turns the virtual remote: deadzone, then a squared response
            // up to 4 rad/s so small deflections steer a pointer precisely. Guest axes
            // (virtual_motion.h): +pitch = nose down, +yaw = turn left; so stick right
            // turns right and stick up tilts up, moving SS's MotionPlus pointer with it.
            const float dead_zone = float(bind.right_deadzone) / 32767.0f;
            const auto motion_axis = [dead_zone](int value) {
                const float dead = std::min(dead_zone, 0.99f);
                const float v = std::clamp(value / 32767.0f, -1.0f, 1.0f);
                if (std::abs(v) <= dead) return 0.0f;
                const float s = (std::abs(v) - dead) / (1.0f - dead);
                return std::copysign(s * s, v);
            };
            const float aim_rate = 4.0f * bind.aim_stick_speed;
            pitch = motion_axis(pad_axis(pad, SDL_CONTROLLER_AXIS_RIGHTY)) * aim_rate;
            yaw = -motion_axis(pad_axis(pad, SDL_CONTROLLER_AXIS_RIGHTX)) * aim_rate;
            // INPUT13: the game's own menu pointer during play (pause menu, map, shop, ...) is a menu screen
            // for the stick too: no sword flicks there (a quick push used to swing and freeze the pointer).
            const bool game_pointer = g_guest_player_present.load() && g_guest_pointer_cursor.load();
            // Sword gestures (motion_gesture.h) while the game reports a player actor (SS: dAcPy_c::LINK):
            // right-stick flick = swing in that direction (with spin_modifier (RT) held: spin attack),
            // thrust (RB), nunchuk_flick (RT press) = Nunchuk flick (shield). Menus and the title keep the
            // pure aim behaviour.
            if (g_guest_player_present.load() && !game_pointer) {
                const float rx = std::clamp(pad_axis(pad, SDL_CONTROLLER_AXIS_RIGHTX) / 32767.0f, -1.0f, 1.0f);
                const float ry = std::clamp(pad_axis(pad, SDL_CONTROLLER_AXIS_RIGHTY) / 32767.0f, -1.0f, 1.0f);
                const bool spin_modifier = pad_action_held(pad, kPadSpinModifier);
                const bool thrust = pad_action_held(pad, kPadThrust);
                const bool nflick = pad_action_held(pad, kPadNunchukFlick);
                auto& gesture = g_virtual_gesture[i];
                float fx = 0, fy = 0;
                if (flick[i].update(rx, -ry, motes[i].motion.dt_seconds, fx, fy)) {
                    if (spin_modifier) gesture.spin(std::abs(fy) > std::abs(fx));
                    else gesture.swing(fx, fy);
                }
                if (flick[i].suppress) pitch = yaw = 0.0f;
                if (thrust && !previous_rb[i]) gesture.thrust();
                if (nflick && !previous_rt[i]) gesture.nunchuk_flick();
                previous_rb[i] = thrust;
                previous_rt[i] = nflick;
            } else {
                // No gestures here; a stick still deflected when play resumes must return first (no swing).
                const float rx = pad_axis(pad, SDL_CONTROLLER_AXIS_RIGHTX) / 32767.0f;
                const float ry = pad_axis(pad, SDL_CONTROLLER_AXIS_RIGHTY) / 32767.0f;
                flick[i] = StickFlick{};
                flick[i].armed = rx * rx + ry * ry < 0.09f;
                previous_rb[i] = pad_action_held(pad, kPadThrust);
                previous_rt[i] = pad_action_held(pad, kPadNunchukFlick);
            }
            // Any deliberate gamepad use takes the aim away from a resting mouse (g_aim_owner).
            const bool stick_active = pitch != 0.0f || yaw != 0.0f;
            // A controller present at start owns the aim until the mouse is actually moved.
            static bool claimed_on_connect[4] = {};
            bool claim = stick_active || !claimed_on_connect[i] || button_claim;
            claimed_on_connect[i] = true;
            if (claim) g_aim_owner.store(1);
            // During the game's point-at-screen step the remote is held level at the centre (like the mouse source).
            const bool recentre = recenter_held || g_guest_point_center.load();
            float level_roll = 0.0f; // SOAK14: roll that levels the remote while it is aimed (rates_toward)
            const bool menu_cursor = g_guest_motionplus_pointer[i].load() &&
                                     (!g_guest_player_present.load() || game_pointer) && g_aim_owner.load() == 1 && dt_ok;
            if (menu_cursor && !menu_prev[i]) {
                // INPUT13: entering a menu, or taking the pointer back from the mouse: the cursor starts where the
                // pointer is (no jump to an old target); an off-screen pointer starts at the centre.
                // SOAK14: the pointer the player sees = ours + the learned game offset (virtual_motion.h PointerAimLoop).
                float px = 0.0f, py = 0.0f;
                if (g_pointer_aim[i].game_pointer(g_virtual_orientation[i], px, py) && std::abs(px) <= 1.0f &&
                    std::abs(py) <= 1.0f) {
                    cursor_x[i] = px;
                    cursor_y[i] = py;
                } else {
                    cursor_x[i] = cursor_y[i] = 0.0f;
                }
            }
            menu_prev[i] = menu_cursor;
            if (menu_cursor) {
                // Stick moves the screen target (full deflection ~1.6 screen-halves/s); the remote is
                // turned onto it with transient rates, so the game's zero-drift correction never
                // absorbs a held stick and the pointer rests where it was left.
                if (recentre) { cursor_x[i] = 0.0f; cursor_y[i] = 0.0f; }
                cursor_x[i] = std::clamp(cursor_x[i] - yaw / 4.0f * 1.6f * dt, -1.0f, 1.0f);
                cursor_y[i] = std::clamp(cursor_y[i] + pitch / 4.0f * 1.6f * dt, -1.0f, 1.0f);
                double target[3];
                // SOAK14: closed loop - aim so that the GAME's pointer (revised by SS during attacks) lands there.
                g_pointer_aim[i].aim(cursor_x[i], cursor_y[i], target);
                float rates[3];
                g_virtual_orientation[i].rates_toward(target, dt, kPointerMotionMaxRateRadS, rates);
                pitch = rates[0];
                yaw = rates[1];
                level_roll = rates[2]; // SOAK14
            } else if (recentre && dt_ok) {
                // In play: steer the virtual remote back to level / screen centre while held.
                double target[3];
                pointer_direction(0.0, 0.0, target);
                float rates[3];
                g_virtual_orientation[i].rates_toward(target, dt, kPointerMotionMaxRateRadS, rates);
                pitch = rates[0];
                yaw = rates[1];
                level_roll = rates[2]; // SOAK14
                cursor_x[i] = cursor_y[i] = 0.0f;
            }
            // Stick clicks leave the existing Nunchuk C (left shoulder) and Z
            // (left trigger) mappings free. These are virtual rates, not sensor data.
            roll = (float(pad_action_held(pad, kPadRollRight)) - float(pad_action_held(pad, kPadRollLeft))) * 4.0f;
            if (roll == 0.0f) roll = level_roll; // SOAK14: an explicit roll input wins
        }
        merge_virtual_motion(motes[i].motion, SDL_GetKeyboardFocus() != nullptr, pitch, yaw, roll);

        if (want_pointer) {
            float dx = 0.0f, dy = 0.0f;

            if (snap.has_gyro) {
                if (snap.gyro_data_ok) {
                    dx = snap.gyro_data[0] * cfg.gyro_sensitivity;
                    dy = snap.gyro_data[1] * cfg.gyro_sensitivity;
                }
            } else {
                int rx = pad_axis(pad, SDL_CONTROLLER_AXIS_RIGHTX);
                int ry = pad_axis(pad, SDL_CONTROLLER_AXIS_RIGHTY);

                if (std::abs(rx) > 8000) dx = (rx / 32767.0f);
                if (std::abs(ry) > 8000) dy = (ry / 32767.0f);
            }
            float speed = (mode == 4)
                ? 0.10f * cfg.gyro_sensitivity
                : 0.05f * cfg.pointer_speed;
            motes[i].ir_x += dx * speed;
            motes[i].ir_y += dy * speed;
            if (motes[i].ir_x < 0.0f) motes[i].ir_x = 0.0f;
            if (motes[i].ir_x > 1.0f) motes[i].ir_x = 1.0f;
            if (motes[i].ir_y < 0.0f) motes[i].ir_y = 0.0f;
            if (motes[i].ir_y > 1.0f) motes[i].ir_y = 1.0f;

            motes[i].accel_x = dx;
            motes[i].accel_y = dy;
            motes[i].accel_z = 1.0f;
        }
        // Sensor requests for the capture: gyro motion and/or the legacy pointer modes' gyro value.
        request_sensors(i, (gyro_active ? kWantGyroAccel : 0) | (want_pointer && snap.has_gyro ? kWantGyroOnly : 0));

        // Wii Remote buttons (input_bindings.h [input.gamepad]; defaults = the fixed mapping before INPUT10).
        static const std::pair<PadAction, uint32_t> kWiiButtons[] = {
            {kPadA, 0x0800}, {kPadB, 0x0400}, {kPadOne, 0x0200}, {kPadTwo, 0x0100}, {kPadPlus, 0x0010},
            {kPadMinus, 0x1000}, {kPadHome, 0x8000}, {kPadUp, 0x0008}, {kPadDown, 0x0004}, {kPadLeft, 0x0001},
            {kPadRight, 0x0002}};
        for (const auto& [action, mask] : kWiiButtons)
            if (pad_action_held(pad, action)) motes[i].buttons |= mask;

        if (pad_button(pad, SDL_CONTROLLER_BUTTON_A)) pads[i].buttons |= 0x0100;
        if (pad_button(pad, SDL_CONTROLLER_BUTTON_B)) pads[i].buttons |= 0x0200;
        if (pad_button(pad, SDL_CONTROLLER_BUTTON_START)) pads[i].buttons |= 0x1000;
        if (pad_button(pad, SDL_CONTROLLER_BUTTON_X)) pads[i].buttons |= 0x0400;
        if (pad_button(pad, SDL_CONTROLLER_BUTTON_Y)) pads[i].buttons |= 0x0800;
        if (pad_button(pad, SDL_CONTROLLER_BUTTON_DPAD_UP)) pads[i].buttons |= 0x0008;
        if (pad_button(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) pads[i].buttons |= 0x0004;
        if (pad_button(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) pads[i].buttons |= 0x0001;
        if (pad_button(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) pads[i].buttons |= 0x0002;
        if (pad_button(pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) pads[i].buttons |= 0x0040;
        if (pad_button(pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) pads[i].buttons |= 0x0020;
        if (pad_axis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > bind.trigger_threshold) pads[i].buttons |= 0x0010;

        int lx = pad_axis(pad, SDL_CONTROLLER_AXIS_LEFTX);
        int ly = pad_axis(pad, SDL_CONTROLLER_AXIS_LEFTY);
        const int left_dead = bind.left_deadzone;
        NunchukState nunchuk;
        nunchuk.available = true;
        if (std::abs(lx) > left_dead)
            nunchuk.stick_x = std::clamp(lx / 32767.0f, -1.0f, 1.0f);
        if (std::abs(ly) > left_dead)
            nunchuk.stick_y = std::clamp(-ly / 32767.0f, -1.0f, 1.0f);
        const float length = std::sqrt(nunchuk.stick_x * nunchuk.stick_x +
                                       nunchuk.stick_y * nunchuk.stick_y);
        if (length > 1.0f) {
            nunchuk.stick_x /= length;
            nunchuk.stick_y /= length;
        }
        if (pad_action_held(pad, kPadC)) nunchuk.buttons |= 0x4000;
        if (pad_action_held(pad, kPadZ)) nunchuk.buttons |= 0x2000;
        merge_nunchuk(motes[i].nunchuk, nunchuk);
        if (pad_in_use(snap, bind)) g_active_device[i].store(int(ActiveDevice::Gamepad)); // CTRL18-INPUT (rumble)
        if (std::abs(lx) > left_dead) pads[i].stick_x = (int8_t)((lx / 32767.0f) * 127.0f);
        if (std::abs(ly) > left_dead) pads[i].stick_y = (int8_t)((-ly / 32767.0f) * 127.0f);

        if (!want_pointer) {
            int rx = pad_axis(pad, SDL_CONTROLLER_AXIS_RIGHTX);
            int ry = pad_axis(pad, SDL_CONTROLLER_AXIS_RIGHTY);
            if (std::abs(rx) > bind.right_deadzone) pads[i].substick_x = (int8_t)((rx / 32767.0f) * 127.0f);
            if (std::abs(ry) > bind.right_deadzone) pads[i].substick_y = (int8_t)((-ry / 32767.0f) * 127.0f);
        }

        pads[i].trigger_l = (uint8_t)((pad_axis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) / 32767.0f) * 255.0f);
        pads[i].trigger_r = (uint8_t)((pad_axis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) / 32767.0f) * 255.0f);
    }
    // INPUT13: say once how the gyro gets the pointer back after the mouse took it (the mouse source runs first
    // in a frame, so compare with the owner at the end of the previous update).
    if (any_gyro && owner_prev == 1 && gyro_owned_before == false && g_aim_owner.load() == 0 && !mouse_note_logged) {
        mouse_note_logged = true;
        std::printf("[Input] The mouse moved, so the mouse aims now. Move the controller or press a button to aim "
                    "with the gyro again.\n");
        std::fflush(stdout);
    }
    owner_prev = g_aim_owner.load();
}

void GamepadSource::update_wii_remote(int i, const PadSnapshot& snap, WiimoteState& mote, bool& any_gyro) {
    const float dt=mote.motion.dt_seconds;
    const bool focused=SDL_GetKeyboardFocus()!=nullptr;
    const bool capable=snap.has_gyro && snap.has_accel;
    // Remove a previously queued virtual swing; the actual remote supplies swings.
    g_virtual_gesture[i].reset();
    mote.buttons=focused ? wii_remote_buttons(snap.buttons) : 0;
    mote.ir_x=mote.ir_y=-1.0f;
    mote.nunchuk=focused ? wii_remote_nunchuk(snap) : NunchukState{};
    mote.motion.available=false;
    g_gyro_drives_channel[i].store(focused && capable);
    if(!focused || !capable) {
        request_sensors(i,0);
        if(sensors_on[i]) {release_sensor_slot(sensor_id[i]);gyro[i].discard();sensors_on[i]=false;}
        if(!capable && !no_sensor_logged[i]) {
            no_sensor_logged[i]=true;
            std::printf("[Input] Wii Remote has no MotionPlus sensor. Skyward Sword requires Wii Remote Plus and a Nunchuk.\n");
        }
        return;
    }
    any_gyro=true;g_aim_owner.store(1);
    if(!sensors_on[i] || sensor_id[i]!=snap.instance_id) {
        if(sensor_id[i]!=snap.instance_id) {gyro[i].reset_bias();calibration_serial[i]=0;}
        sensor_id[i]=snap.instance_id;gyro[i].reset();gyro[i].grip_tilt=0;
        // SDL normalizes Wii and gamepad sensor axes alike. A real remote keeps
        // its actual pose; gamepad grip offsets and gravity-relative aim do not apply.
        gyro[i].filter_aim=false;
        request_sensors(i,kWantGyroAccel);
        drain_sensor_events(sensor_id[i],scratch);
        sensors_on[i]=true;recenter_left_s[i]=0;recenter_prev[i]=false;
        push_controller_event(ControllerEventKind::GyroOn,i,snap.name,"Wii Remote",true);
        std::printf("[Input] Wii Remote Plus motion active: original buttons, physical MotionPlus and %s. Down centres the pointer.\n",
                    snap.has_nunchuk ? "physical Nunchuk" : "no Nunchuk (attach one)");
        std::fflush(stdout);
    }
    request_sensors(i,kWantGyroAccel);
    drain_sensor_events(sensor_id[i],scratch);
    float bias[3];if(calibration_bias(sensor_id[i],calibration_serial[i],bias))gyro[i].set_bias(bias);
    gyro[i].deadband=input_bindings().gyro_deadband;
    for(const auto& e:scratch)gyro[i].add(e);
    if(mote.nunchuk.available) {
        float a[3];
        if(read_nunchuk_accel(sensor_id[i],a)) {
            VirtualGestureController::clamp_accel(a,kNunchukAccelRangeG);
            mote.nunchuk.accel_x=a[0];mote.nunchuk.accel_y=a[1];mote.nunchuk.accel_z=a[2];
            double speed=0;
            for(int axis=0;axis<3;++axis) {
                const double delta=a[axis]-wii_nunchuk_previous[i][axis];speed+=delta*delta;
                wii_nunchuk_previous[i][axis]=a[axis];
            }
            mote.nunchuk.accel_speed=wii_nunchuk_seen[i] ? float(std::sqrt(speed)) : 0;
            wii_nunchuk_seen[i]=true;
        } else {wii_nunchuk_seen[i]=false;}
    }
    auto& frame=g_measured_motion[i];frame.sequence=mote.motion.sequence;frame.count=0;
    if(!(dt>0 && dt<=0.1f)) {gyro[i].discard();return;}
    const auto n=gyro[i].produce(dt,frame.steps.data(),frame.steps.size());
    const bool down=(mote.buttons & 0x0004)!=0;
    const bool ir=snap.ir_valid && std::isfinite(snap.ir_x) && std::isfinite(snap.ir_y);
    if(ir && (!wii_ir_reference_set[i] || (down && !recenter_prev[i]) || g_guest_point_center.load())) {
        // The current aim is the screen centre, regardless of the bar being
        // above/below the display. Lost visibility never silently re-centres it.
        wii_ir_reference[i][0]=snap.ir_x;wii_ir_reference[i][1]=snap.ir_y;
        wii_ir_reference_set[i]=true;
    }
    const float ir_x=ir ? std::clamp(0.5f+snap.ir_x-wii_ir_reference[i][0],0.0f,1.0f) : -1.0f;
    const float ir_y=ir ? std::clamp(0.5f+snap.ir_y-wii_ir_reference[i][1],0.0f,1.0f) : -1.0f;
    mote.ir_horizon_x=ir ? snap.ir_horizon_x : 1.f;
    mote.ir_horizon_y=ir ? snap.ir_horizon_y : 0.f;
    if(down && !recenter_prev[i]) recenter_left_s[i]=0.6;
    recenter_prev[i]=down;
    if(g_guest_point_center.load() || recenter_left_s[i]>0) {
        // Match the game's initial point-at-screen step without physical IR.
        double target[3];pointer_direction(0,0,target);float rates[3];
        g_virtual_orientation[i].rates_toward(target,dt,kPointerMotionMaxRateRadS,rates);
        merge_virtual_motion(mote.motion,true,rates[0],rates[1],rates[2]);
        frame.sequence=0;recenter_left_s[i]=std::max(0.0,recenter_left_s[i]-dt);
        mote.ir_x=ir ? ir_x : g_guest_point_center.load() ? 0.5f : -1.f;
        mote.ir_y=ir ? ir_y : g_guest_point_center.load() ? 0.5f : -1.f;
    } else {
        mote.ir_x=ir_x;mote.ir_y=ir_y;frame.count=n;
        // Zero fresh packets is explicitly retained by InputManager, rather
        // than replaying the last measured rotation at the host poll rate.
        merge_virtual_motion(mote.motion,true,0,0,0);
    }
}

// CTRL18-INPUT: the "switch-hd" preset for one controller. Physical inputs -> intents ([input.gamepad_hd]) ->
// HdPadTranslator (hd_translator.h: game context -> Wii Remote / Nunchuk signals). The gyro, when "gyro aiming" is on
// (hold the gyro_toggle 1 s, or [input] gyro_aim), also aims menus and rotates the boss key; the
// accelerometer is never passed, so hand motion cannot trigger sword flags.
void GamepadSource::update_switch_hd(int i, const PadSnapshot& snap, GameCubePadState& gc, WiimoteState& mote,
                                     bool& any_gyro) {
    const InputBindings& bind = input_bindings();
    PadState pad;
    pad.snap = &snap;
    pad.trigger_threshold = bind.trigger_threshold;
    const float dt = mote.motion.dt_seconds;
    const bool dt_ok = dt > 0.0f && dt <= 0.1f;

    // gyro_toggle (default touchpad / -+ held 1 s): gyro aiming on/off. A held multi-button combo is not passed on.
    if (const InputCombo* toggle = hd_action_combo(pad, kHdGyroToggle)) {
        if (toggle->n >= 2)
            for (int k = 0; k < toggle->n; ++k) {
                const BoundInput& in = toggle->in[size_t(k)];
                if (in.kind == BoundInput::Button && in.code < 32) pad.suppressed |= 1u << in.code;
                if (in.kind == BoundInput::Face) pad.suppressed |= 1u << face_button_for(in.code, snap.type);
            }
        if (dt_ok) toggle_held_s[i] += dt;
        if (!toggle_fired[i] && toggle_held_s[i] >= 1.0) {
            toggle_fired[i] = true;
            const bool on = g_gamepad_motion_mode.load() != 1;
            g_gamepad_motion_mode.store(on ? 1 : 0);
            std::printf("[Input] Gyro aiming %s (switch-hd; gyro_toggle held 1 s on controller %d, %s)%s\n",
                        on ? "ON" : "OFF", i, snap.name,
                        on && !(snap.has_gyro && snap.has_accel) ? " - this controller has no gyroscope" : "");
            std::fflush(stdout);
            request_rumble(i, on ? 0x7000 : 0x3000, on ? 0x7000 : 0x3000, on ? 200 : 100);
        }
    } else {
        toggle_held_s[i] = 0.0;
        toggle_fired[i] = false;
    }

    PadIntents in;
    for (int a = 0; a < kHdActionCount; ++a) in.held[a] = hd_action_combo(pad, HdAction(a)) != nullptr;
    // Left stick: deadzone, unit circle (as the classic Nunchuk stick).
    const int lx = pad_axis(pad, SDL_CONTROLLER_AXIS_LEFTX), ly = pad_axis(pad, SDL_CONTROLLER_AXIS_LEFTY);
    if (std::abs(lx) > bind.left_deadzone) in.lx = std::clamp(lx / 32767.0f, -1.0f, 1.0f);
    if (std::abs(ly) > bind.left_deadzone) in.ly = std::clamp(-ly / 32767.0f, -1.0f, 1.0f);
    if (const float len = std::sqrt(in.lx * in.lx + in.ly * in.ly); len > 1.0f) { in.lx /= len; in.ly /= len; }
    in.rx = std::clamp(pad_axis(pad, SDL_CONTROLLER_AXIS_RIGHTX) / 32767.0f, -1.0f, 1.0f);
    in.ry = std::clamp(-pad_axis(pad, SDL_CONTROLLER_AXIS_RIGHTY) / 32767.0f, -1.0f, 1.0f);
    in.right_dead = float(bind.right_deadzone) / 32767.0f;
    in.aim_speed = bind.aim_stick_speed;
    in.dt = dt_ok ? dt : 0.0;

    const GameContext ctx = g_guest_context.get();
    const LiveFlags live = LiveFlags::read(i);
    const Situation situation = classify(ctx, live);
    bool gyro_claim = false;
    // Preserve the measured angular integral across different sensor/host rates.
    const bool hybrid = g_gamepad_motion_mode.load() == 1 && snap.has_gyro && snap.has_accel;
    if (hybrid) {
        const SDL_JoystickID id = snap.instance_id;
        if (!sensors_on[i] || id != sensor_id[i]) {
            install_sensor_watch();
            if (sensor_id[i] != id) {
                if (sensor_id[i] >= 0) release_sensor_slot(sensor_id[i]);
                gyro[i].reset_bias();
                calibration_serial[i]=0;gravity_aim[i].reset();
            }
            request_sensors(i, kWantGyroAccel);
            drain_sensor_events(id, scratch);
            gyro[i].reset();
            gyro_claim_s[i] = 0;
            gyro[i].deadband = bind.gyro_deadband;
            gyro[i].grip_tilt = double(bind.gyro_grip_tilt_deg) * 3.14159265358979323846 / 180.0;
            sensor_id[i] = id;
            sensors_on[i] = true;
            std::printf("[Input] Gyro aiming active on controller %d (%s): in first person and while aiming an item the "
                        "controller's motion aims too; swings stay on the right stick. Y recentres.\n", i, snap.name);
            std::fflush(stdout);
            push_controller_event(ControllerEventKind::GyroOn, i, snap.name, pad_type_name(snap.type), true);
        }
        drain_sensor_events(id, scratch);
        float manual_bias[3];if(calibration_bias(id,calibration_serial[i],manual_bias))gyro[i].set_bias(manual_bias);
        for (const auto& e : scratch) gyro[i].add(e);
        gyro[i].filter_aim = situation == Situation::Aim || situation == Situation::Menu || situation == Situation::Event;
        gyro[i].deadband = bind.gyro_deadband;
        if (dt_ok) {
            MeasuredSubStep steps[kMaxMeasuredSubSteps];
            const size_t n = gyro[i].produce(dt, steps, kMaxMeasuredSubSteps);
            double sum[3] = {0, 0, 0}, total = 0;
            for (size_t k = 0; k < n; ++k) {
                if (!(steps[k].dt > 0.0f)) continue;
                float aim_rate[3];gravity_aim[i].map(steps[k].rate,steps[k].accel,steps[k].dt,aim_rate);
                const bool mapped=bind.gyro_player_space && (situation==Situation::Aim || situation==Situation::Menu || situation==Situation::Event);
                for (int a = 0; a < 3; ++a) sum[a] += double(mapped ? aim_rate[a] : steps[k].rate[a]) * steps[k].dt;
                total += steps[k].dt;
            }
            if (total > 0) {
                in.gyro_valid = true;
                for (int a = 0; a < 3; ++a) in.gyro_rate[a] = float(sum[a] / dt);
                const double magnitude = std::sqrt(sum[0]*sum[0]+sum[1]*sum[1]+sum[2]*sum[2]) / total;
                if (!scratch.empty()) {
                    gyro_claim_s[i] = magnitude > 0.12 ? gyro_claim_s[i]+total : 0;
                    gyro_claim = gyro_claim_s[i] >= 0.035; // measured sensor time; empty host polls are not stops
                }
            }
        } else {
            gyro[i].discard();
        }
        const bool recentre = in.held[kHdRecentre];
        if (recentre && !hd_recentre_prev[i]) {
            double grip = 0.0;
            if (gyro[i].grip_from_gravity(grip)) gyro[i].grip_tilt = grip;
            in.gyro_recentre = true;
            push_controller_event(ControllerEventKind::GyroRecentred, i, snap.name, pad_type_name(snap.type), true);
        }
        hd_recentre_prev[i] = recentre;
        any_gyro = true;
    } else if (sensors_on[i]) {
        request_sensors(i, 0);
        release_sensor_slot(sensor_id[i]);
        gyro[i].discard();
        sensors_on[i] = false;
        std::printf("[Input] Gyro aiming off on controller %d: the right stick aims.\n", i);
        std::fflush(stdout);
        push_controller_event(ControllerEventKind::GyroOff, i, snap.name, pad_type_name(snap.type),
                              snap.has_gyro && snap.has_accel);
    }
    in.gyro_aim = hybrid;
    g_gyro_drives_channel[i].store(false); // the hybrid never replaces the virtual remote's sensors
    if (!hybrid && snap.has_gyro && snap.has_accel && !gyro_hint_logged) {
        gyro_hint_logged = true;
        std::printf("[Input] Tip: %s has a gyroscope. Hold the touchpad (or - and +) 1 s to also aim with the gyro "
                    "(first person, bow, slingshot); swings stay on the right stick.\n", snap.name);
        std::fflush(stdout);
        ctrl18_set_gyro_hint(true);
    }

    g_ctrl18_flight_invert_y.store(bind.flight_invert_y);
    auto& t = hd[i];
    t.a_does_not_dash = bind.a_does_not_dash;
    t.sword().tuning.fire_on_release = bind.fire_on_release;
    in.owns_pointer = gyro_claim || g_aim_owner.load() == 1 || !hd_claimed[i] || pad_in_use(snap, bind);
    WiiOut out;
    t.step(in, ctx, live, g_virtual_gesture[i], g_virtual_orientation[i], g_pointer_aim[i], out);
    if (i == 0) {
        // Free camera (CTRL18-CAM seam): the right stick while "camera" is held.
        const bool cam = in.held[kHdCamera] && out.situation != Situation::Menu && out.situation != Situation::Puzzle;
        ctrl18_feed_camera(cam ? aim_axis(in.rx, in.right_dead) : 0.0f, cam ? aim_axis(in.ry, in.right_dead) : 0.0f);
        ctrl18_set_status("switch-hd", out.situation, hybrid);
    }
    // A controller present at start owns the aim until the mouse is actually moved (as the classic path).
    if (out.claim_aim || gyro_claim || !hd_claimed[i]) {
        if (g_aim_owner.exchange(1) == 0) {
            // The mouse source ran first this update. Its old absolute correction
            // must not win merge_virtual_motion's strongest-axis arbitration.
            mote.motion.gyro_x_rad_s = mote.motion.gyro_y_rad_s = mote.motion.gyro_z_rad_s = 0.0f;
        }
    }
    hd_claimed[i] = true;
    if (pad_in_use(snap, bind)) g_active_device[i].store(int(ActiveDevice::Gamepad)); // CTRL18-CAM rumble routing
    merge_virtual_motion(mote.motion, SDL_GetKeyboardFocus() != nullptr, out.pitch, out.yaw, out.roll);
    mote.buttons |= out.buttons;
    NunchukState nunchuk;
    nunchuk.available = true;
    nunchuk.stick_x = out.stick_x;
    nunchuk.stick_y = out.stick_y;
    nunchuk.buttons = out.nunchuk_buttons;
    merge_nunchuk(mote.nunchuk, nunchuk);

    // GameCube pad (unchanged mapping; SS does not read it).
    if (pad_button(pad, SDL_CONTROLLER_BUTTON_A)) gc.buttons |= 0x0100;
    if (pad_button(pad, SDL_CONTROLLER_BUTTON_B)) gc.buttons |= 0x0200;
    if (pad_button(pad, SDL_CONTROLLER_BUTTON_START)) gc.buttons |= 0x1000;
    if (pad_button(pad, SDL_CONTROLLER_BUTTON_X)) gc.buttons |= 0x0400;
    if (pad_button(pad, SDL_CONTROLLER_BUTTON_Y)) gc.buttons |= 0x0800;
    if (pad_button(pad, SDL_CONTROLLER_BUTTON_DPAD_UP)) gc.buttons |= 0x0008;
    if (pad_button(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) gc.buttons |= 0x0004;
    if (pad_button(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) gc.buttons |= 0x0001;
    if (pad_button(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) gc.buttons |= 0x0002;
    if (pad_button(pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) gc.buttons |= 0x0040;
    if (pad_button(pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) gc.buttons |= 0x0020;
    if (pad_axis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > bind.trigger_threshold) gc.buttons |= 0x0010;
    if (std::abs(lx) > bind.left_deadzone) gc.stick_x = (int8_t)((lx / 32767.0f) * 127.0f);
    if (std::abs(ly) > bind.left_deadzone) gc.stick_y = (int8_t)((-ly / 32767.0f) * 127.0f);
    const int rxa = pad_axis(pad, SDL_CONTROLLER_AXIS_RIGHTX), rya = pad_axis(pad, SDL_CONTROLLER_AXIS_RIGHTY);
    if (std::abs(rxa) > bind.right_deadzone) gc.substick_x = (int8_t)((rxa / 32767.0f) * 127.0f);
    if (std::abs(rya) > bind.right_deadzone) gc.substick_y = (int8_t)((-rya / 32767.0f) * 127.0f);
    gc.trigger_l = (uint8_t)((pad_axis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) / 32767.0f) * 255.0f);
    gc.trigger_r = (uint8_t)((pad_axis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) / 32767.0f) * 255.0f);
}

// ---------------------------------------------------------------------------------------------------------
// INPUT13: the controller thread.
namespace {
struct ControllerThread {
    std::thread thread;
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> quit{false}, finished{false};
};
ControllerThread& controller_thread() {
    static ControllerThread* t = new ControllerThread; // never freed: may be detached on a stuck driver
    return *t;
}
void mark_busy(PadBusy what) {
    auto& hub = pad_hub();
    hub.busy_what.store(int(what));
    hub.busy_since_ms.store(what == PadBusy::None ? 0 : std::max<uint64_t>(pad_now_ms(), 1));
}
// Window messages of this thread (SDL's HIDAPI device-change window, the Windows joystick notification and
// raw-input windows are created by the thread that initialises the joystick subsystem and only that thread
// can dispatch them; SDL_PumpEvents here would also run the video pump on the wrong thread).
void pump_thread_messages() {
#ifdef _WIN32
    MSG msg;
    for (int budget = 0; budget < 256 && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE); ++budget) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
#endif
}
// ~2 ms between polls. Windows 11 ignores the 1 ms timer resolution for a process without a visible window
// (minimised game: Sleep(2) became 10-16 ms in the smoke check), so a high-resolution waitable timer is used;
// a window message (device change) ends the wait early.
class PollTimer {
public:
#ifdef _WIN32
    PollTimer() { timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS); }
    ~PollTimer() { if (timer_) CloseHandle(timer_); }
    void wait() {
        LARGE_INTEGER due;
        due.QuadPart = -20000; // 2 ms, relative, 100 ns units
        if (timer_ && SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) {
            MsgWaitForMultipleObjects(1, &timer_, FALSE, 20, QS_ALLINPUT);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
private:
    HANDLE timer_ = nullptr;
#else
    void wait() { std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
#endif
};
void controller_thread_main(ControllerApi api) {
    auto& hub = pad_hub();
    auto& t = controller_thread();
    const uint64_t t0 = pad_now_ms();
    mark_busy(PadBusy::Init);
    const int rc = api.init ? api.init() : 0;
    mark_busy(PadBusy::None);
    if (rc < 0) {
        std::printf("[Input] Controllers unavailable: SDL game controller init failed (%s). Keyboard and mouse work.\n",
                    api.error ? api.error() : "?");
        std::fflush(stdout);
        hub.init_failed.store(true);
        {
            std::lock_guard lock(t.mutex);
            hub.init_done.store(true);
        }
        t.cv.notify_all();
        t.finished.store(true);
        return;
    }
    bool first = true;
    PollTimer poll_timer;
    // NWII_PAD_TRACE=1 (diagnostic): loop rate and SDL_JoystickUpdate / capture cost every 5 s.
    const bool trace = [] { const char* e = std::getenv("NWII_PAD_TRACE"); return e && e[0] == '1'; }();
    using TraceClock = std::chrono::steady_clock;
    auto trace_t0 = TraceClock::now();
    double trace_update_ms = 0, trace_update_max = 0, trace_capture_ms = 0;
    long trace_loops = 0;
    while (!t.quit.load()) {
        if (api.pump_messages) api.pump_messages();
        else pump_thread_messages();
        const auto u0 = TraceClock::now();
        mark_busy(PadBusy::Update);
        if (api.update) api.update();
        const auto u1 = TraceClock::now();
        mark_busy(PadBusy::Capture);
        capture_controllers(&api);
        mark_busy(PadBusy::None);
        if (trace) {
            const double um = std::chrono::duration<double, std::milli>(u1 - u0).count();
            trace_update_ms += um;
            trace_update_max = std::max(trace_update_max, um);
            trace_capture_ms += std::chrono::duration<double, std::milli>(TraceClock::now() - u1).count();
            const double span = std::chrono::duration<double>(TraceClock::now() - trace_t0).count();
            if (++trace_loops > 0 && span >= 5.0) {
                std::printf("[Input] pad trace: %.0f loops/s, SDL_JoystickUpdate avg %.2f ms max %.1f ms, capture avg %.2f ms\n",
                            trace_loops / span, trace_update_ms / trace_loops, trace_update_max, trace_capture_ms / trace_loops);
                std::fflush(stdout);
                trace_t0 = TraceClock::now();
                trace_update_ms = trace_update_max = trace_capture_ms = 0;
                trace_loops = 0;
            }
        }
        if (first) {
            first = false;
            int n = 0;
            for (const auto& d : capture_state().slot) n += d.controller ? 1 : 0;
            std::printf("[Input] Controller thread: start-up scan took %llu ms, %d controller(s); SDL joystick updates "
                        "now run off the main thread.\n", (unsigned long long)(pad_now_ms() - t0), n);
            std::fflush(stdout);
            {
                std::lock_guard lock(t.mutex);
                hub.init_done.store(true);
            }
            t.cv.notify_all();
        }
        poll_timer.wait();
    }
    for (auto& d : capture_state().slot) {
        if (d.controller && api.close) api.close(d.controller);
        d = CaptureDevice{};
    }
    if (api.quit) api.quit();
    t.finished.store(true);
}
} // namespace

bool start_controller_thread(const ControllerApi& api, double wait_init_s) {
    auto& t = controller_thread();
    auto& hub = pad_hub();
    if (t.thread.joinable() || hub.threaded.load()) return hub.init_done.load();
    hub.threaded.store(true); // from now on only that thread calls SDL joystick functions
    t.quit.store(false);
    t.finished.store(false);
    t.thread = std::thread(controller_thread_main, api);
    std::unique_lock lock(t.mutex);
    t.cv.wait_for(lock, std::chrono::milliseconds(int64_t(std::max(0.0, wait_init_s) * 1000.0)),
                  [&] { return hub.init_done.load(); });
    return hub.init_done.load();
}

bool stop_controller_thread(double wait_s) {
    auto& t = controller_thread();
    if (!t.thread.joinable()) return true;
    t.quit.store(true);
    const uint64_t deadline = pad_now_ms() + uint64_t(std::max(0.0, wait_s) * 1000.0);
    while (!t.finished.load() && pad_now_ms() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (t.finished.load()) {
        t.thread.join();
        return true;
    }
    std::printf("[Input] The controller thread is still inside a device driver call; it is left behind (no SDL_Quit).\n");
    std::fflush(stdout);
    t.thread.detach();
    return false;
}

bool controller_thread_running() {
    auto& t = controller_thread();
    return t.thread.joinable() && !t.finished.load();
}

}
