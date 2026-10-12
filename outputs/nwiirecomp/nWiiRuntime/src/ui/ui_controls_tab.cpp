// CTRL18-UI (T8): the "Besturing" tab, the rebind capture, the live test panel and the glyph-set device tracker
// (see ui_controls_tab.h). Data: ui_controls.h (tables, names, conflicts), ui_settings_desc.h (labels, help).
#include "ui_controls_tab.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "input/controller_events.h"
#include "../input/gyro_calibration.h"
#include "../input/hd_translator.h" // path: CTRL18-CAM (lib include path)
#include "runtime/gx/glyph_sets.h"
#include "runtime/gx/hd_textures.h"
#include "runtime/ui_config.h"
#include "ui_controls.h"
#include "ui_settings_desc.h"

namespace nwii::ui {

namespace {
namespace ct = controls;
using glyphs::Device;

struct CaptureState {
    bool active = false;
    ct::TableId table = ct::TableId::PadHd;
    std::string action;
    uint32_t t0 = 0;
};
struct TabState {
    CaptureState cap;
    std::string msg; // result of the last capture (Dutch)
    bool msg_err = false;
    ct::DeviceTracker dev;
    std::string glyph_folder = "\x01"; // last folder sent to the loader (\x01 = none yet)
    // live test panel (from the SDL events the main thread sees; no SDL controller call here)
    float axis[6] = {};
    uint32_t pad_held = 0;   // bit per SDL_GameControllerButton
    std::string last_input, last_device;
    uint32_t last_input_t = 0;
    int bind_view = -1;      // 0 controller table, 1 keyboard table (-1 = by the last used device)
    bool gyro_hint_given = false;
    int gyro_cal_slot = 0;
    uint32_t gyro_hint_check_t = 0;
};
TabState& T() {
    static TabState t;
    return t;
}

const char* FirstPadType(const char** name = nullptr) {
    static runtime::input::ControllerStatus st[4];
    runtime::input::controller_status(st);
    for (const auto& c : st)
        if (c.connected) {
            if (name) *name = c.name;
            return c.type;
        }
    if (name) *name = "";
    return "";
}
bool PadConnected() { return *FirstPadType() != 0; }
bool NintendoPad() {
    const char* t = FirstPadType();
    return std::strstr(t, "Switch") || std::strstr(t, "Joy-Con");
}
Device PadFamily() { return PadConnected() ? glyphs::DeviceFromType(FirstPadType()) : Device::Xbox; }
const char* FamilyNl(Device d) {
    switch (d) {
    case Device::PlayStation: return "PlayStation";
    case Device::Switch: return "Nintendo Switch";
    case Device::Xbox: return "Xbox";
    case Device::Keyboard: return "Toetsenbord + muis";
    }
    return "";
}

Settings::BindMap& Map(Settings& s, ct::TableId t) {
    switch (t) {
    case ct::TableId::PadHd: return s.bind_pad_hd;
    case ct::TableId::PadClassic: return s.bind_pad_classic;
    case ct::TableId::KbmHd: return s.bind_kbm;
    case ct::TableId::KbClassic: return s.bind_kb_classic;
    }
    return s.bind_pad_hd;
}
const Settings::BindMap& Map(const Settings& s, ct::TableId t) { return Map(const_cast<Settings&>(s), t); }

ct::TableId TableFromSection(const std::string& sec, bool& ok) {
    ok = true;
    for (ct::TableId t : {ct::TableId::PadHd, ct::TableId::PadClassic, ct::TableId::KbmHd, ct::TableId::KbClassic})
        if (sec == ct::Table(t).section) return t;
    ok = false;
    return ct::TableId::PadHd;
}

std::string Join(const std::vector<std::string>& v, bool pad, Device d) {
    std::string out;
    for (const std::string& x : v) out += (out.empty() ? "" : "  /  ") + ct::InputLabel(x, pad, d);
    return out.empty() ? std::string("(geen)") : out;
}

// The captured input replaces the action's inputs; back at the defaults = no override (a clean settings file).
void Assign(Settings& edit, ct::TableId t, const std::string& action, const std::string& input) {
    for (const ct::Row& r : ct::Rows(t))
        if (r.name == action) {
            Settings::BindMap& m = Map(edit, t);
            if (r.defaults.size() == 1 && ct::Identity(r.defaults[0], ct::Table(t).pad) == ct::Identity(input, ct::Table(t).pad))
                m.erase(action);
            else
                m[action] = {input};
            return;
        }
}

std::string ActionHelp(ct::TableId t, const std::string& action) {
    for (const ct::Row& r : ct::Rows(t))
        if (r.name == action) return r.help;
    return action;
}

void EndCapture(const std::string& msg, bool err) {
    TabState& s = T();
    s.cap.active = false;
    s.msg = msg;
    s.msg_err = err;
}

void NoteLive(const SDL_Event& e) {
    TabState& s = T();
    switch (e.type) {
    case SDL_CONTROLLERAXISMOTION:
        if (e.caxis.axis < 6) s.axis[e.caxis.axis] = std::clamp(e.caxis.value / 32767.0f, -1.0f, 1.0f);
        break;
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP:
        if (e.cbutton.button < 32) {
            if (e.type == SDL_CONTROLLERBUTTONDOWN) s.pad_held |= 1u << e.cbutton.button;
            else s.pad_held &= ~(1u << e.cbutton.button);
        }
        if (e.type == SDL_CONTROLLERBUTTONDOWN) {
            const std::string n = ct::PadButtonName(e.cbutton.button, false, NintendoPad());
            s.last_input = n.empty() ? std::string("knop ") + std::to_string(e.cbutton.button) : ct::PadLabel(n, PadFamily());
            s.last_device = FamilyNl(PadFamily());
            s.last_input_t = SDL_GetTicks();
        }
        break;
    case SDL_KEYDOWN:
        if (!e.key.repeat) {
            const std::string n = ct::KeyName(e.key.keysym.scancode);
            s.last_input = n.empty() ? std::string("toets ") + std::to_string(int(e.key.keysym.scancode)) : ct::KeyLabel(n);
            s.last_device = "Toetsenbord";
            s.last_input_t = SDL_GetTicks();
        }
        break;
    case SDL_MOUSEBUTTONDOWN: {
        const std::string n = ct::MouseName(e.button.button);
        if (!n.empty()) {
            s.last_input = ct::KeyLabel(n);
            s.last_device = "Muis";
            s.last_input_t = SDL_GetTicks();
        }
        break;
    }
    default:
        break;
    }
}

// ------------------------------------------------------------------------------------------- drawing helpers
void Keycap(const std::string& text, bool lit) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 sz = ImGui::CalcTextSize(text.c_str());
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float pad = 5.0f, h = sz.y + 4.0f;
    dl->AddRectFilled(p, ImVec2(p.x + sz.x + pad * 2, p.y + h), lit ? IM_COL32(120, 92, 30, 255) : IM_COL32(48, 42, 34, 255), 4.0f);
    dl->AddRect(p, ImVec2(p.x + sz.x + pad * 2, p.y + h), IM_COL32(200, 170, 110, 200), 4.0f);
    dl->AddText(ImVec2(p.x + pad, p.y + 2.0f), IM_COL32(245, 235, 210, 255), text.c_str());
    ImGui::Dummy(ImVec2(sz.x + pad * 2, h));
}

bool PresetCard(const char* id, const char* title, const char* line1, const char* line2, bool selected, ImVec2 size,
                const ControlsTabHost& h) {
    ImGui::PushID(id);
    const bool clicked = ImGui::Selectable("##card", selected, ImGuiSelectableFlags_None, size);
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRect(a, b, selected ? IM_COL32(236, 200, 120, 255) : IM_COL32(120, 105, 80, 160), 6.0f, 0, selected ? 2.5f : 1.0f);
    const float x = a.x + 10 * h.s;
    float y = a.y + 6 * h.s;
    if (h.f_bold) ImGui::PushFont(h.f_bold);
    dl->AddText(ImVec2(x, y), selected ? IM_COL32(244, 214, 140, 255) : IM_COL32(235, 225, 205, 255), title);
    y += ImGui::GetFontSize() + 3 * h.s;
    if (h.f_bold) ImGui::PopFont();
    if (h.f_small) ImGui::PushFont(h.f_small);
    const float wrap = b.x - x - 8 * h.s;
    for (const char* l : {line1, line2}) {
        if (!l || !*l) continue;
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(x, y), IM_COL32(200, 190, 170, 255), l, nullptr, wrap);
        y += ImGui::CalcTextSize(l, nullptr, false, wrap).y + 2 * h.s;
    }
    if (h.f_small) ImGui::PopFont();
    ImGui::PopID();
    if (h.track) h.track(std::string("Preset ") + title + (std::strcmp(id, "kb_classic") == 0 ? " (toetsenbord)" : ""));
    return clicked;
}

void Tip(const char* id) {
    if (const SettingDesc* d = FindSetting(id)) ImGui::SetItemTooltip("%s", d->help);
}
const char* Label(const char* id) {
    const SettingDesc* d = FindSetting(id);
    return d ? d->label : id;
}

void StickWidget(const char* name, float x, float y, float s) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = 26.0f * s;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 c(p.x + r + 2, p.y + r + 2);
    dl->AddCircleFilled(c, r, IM_COL32(40, 35, 28, 255));
    dl->AddCircle(c, r, IM_COL32(200, 170, 110, 200), 0, 1.5f);
    dl->AddCircleFilled(ImVec2(c.x + x * (r - 6), c.y + y * (r - 6)), 6.0f * s, IM_COL32(236, 200, 120, 255));
    dl->AddText(ImVec2(c.x - ImGui::CalcTextSize(name).x * 0.5f, c.y + r + 3), IM_COL32(220, 210, 190, 255), name);
    ImGui::Dummy(ImVec2(2 * r + 4, 2 * r + 4 + ImGui::GetFontSize() + 4));
}

void TriggerWidget(const char* name, float v, float s) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = 14.0f * s, hgt = 52.0f * s;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + hgt), IM_COL32(40, 35, 28, 255), 3.0f);
    dl->AddRectFilled(ImVec2(p.x, p.y + hgt * (1 - std::max(0.0f, v))), ImVec2(p.x + w, p.y + hgt), IM_COL32(236, 200, 120, 255), 3.0f);
    dl->AddText(ImVec2(p.x - 2, p.y + hgt + 3), IM_COL32(220, 210, 190, 255), name);
    ImGui::Dummy(ImVec2(w + 16 * s, hgt + ImGui::GetFontSize() + 6));
}

// ------------------------------------------------------------------------------------------- binding table
void BindingTable(Settings& edit, ct::TableId t, const ControlsTabHost& h) {
    TabState& st = T();
    const ct::TableInfo ti = ct::Table(t);
    const Device dev = ti.pad ? PadFamily() : Device::Keyboard;
    const std::vector<ct::Row> rows = ct::Rows(t);
    Settings::BindMap& map = Map(edit, t);
    const std::vector<ct::Conflict> conflicts = ct::Conflicts(rows, map, ti.pad);
    std::vector<std::string> groups = ct::GroupOrder();
    for (const ct::Row& r : rows)
        if (std::find(groups.begin(), groups.end(), r.group) == groups.end()) groups.push_back(r.group);
    if (h.f_small) ImGui::PushFont(h.f_small);
    for (const std::string& g : groups) {
        bool any = false;
        for (const ct::Row& r : rows) any = any || r.group == g;
        if (!any) continue;
        if (h.f_bold) ImGui::PushFont(h.f_bold);
        ImGui::TextColored(ImVec4(0.925f, 0.784f, 0.470f, 1.0f), "%s", g.c_str());
        if (h.f_bold) ImGui::PopFont();
        const std::string tid = std::string("##bind_") + ti.section + g;
        if (!ImGui::BeginTable(tid.c_str(), 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp))
            continue;
        ImGui::TableSetupColumn("Actie", ImGuiTableColumnFlags_WidthStretch, 1.5f);
        ImGui::TableSetupColumn("Knop", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("##knoppen", ImGuiTableColumnFlags_WidthFixed, 150.0f * h.s);
        for (const ct::Row& r : rows) {
            if (r.group != g) continue;
            const bool overridden = map.count(r.name) > 0;
            const bool capturing = st.cap.active && st.cap.table == t && st.cap.action == r.name;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextWrapped("%s", r.help.c_str());
            for (const ct::Conflict& c : conflicts)
                if (std::find(c.actions.begin(), c.actions.end(), r.name) != c.actions.end()) {
                    std::string others;
                    for (const std::string& a : c.actions)
                        if (a != r.name) others += (others.empty() ? "" : "; ") + ActionHelp(t, a);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.62f, 0.30f, 1.0f));
                    ImGui::TextWrapped("Let op: %s wordt ook gebruikt voor: %s", ct::InputLabel(c.input, ti.pad, dev).c_str(),
                                       others.c_str());
                    ImGui::PopStyleColor();
                }
            ImGui::TableSetColumnIndex(1);
            if (capturing) {
                const float a = 0.55f + 0.45f * std::sin(float(ImGui::GetTime()) * 6.0f);
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.45f, a), ti.pad ? "druk op een knop... (Esc = annuleren)"
                                                                         : "druk op een toets of muisknop... (Esc = annuleren)");
            } else {
                const std::vector<std::string> eff = ct::Effective(r, map);
                if (eff.empty()) ImGui::TextDisabled("(geen)");
                for (size_t i = 0; i < eff.size(); ++i) {
                    if (i) ImGui::SameLine();
                    Keycap(ct::InputLabel(eff[i], ti.pad, dev), overridden);
                }
            }
            ImGui::TableSetColumnIndex(2);
            ImGui::PushID((std::string(ti.section) + ":" + r.name).c_str());
            if (ImGui::SmallButton(capturing ? "Annuleren" : "Wijzig")) {
                if (capturing) {
                    EndCapture("Geannuleerd.", false);
                } else {
                    st.cap = {true, t, r.name, SDL_GetTicks()};
                    st.msg.clear();
                }
            }
            if (h.track) h.track(std::string("Wijzig:") + ti.section + ":" + r.name);
            if (overridden) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Standaard")) map.erase(r.name);
                ImGui::SetItemTooltip("Terug naar: %s", Join(r.defaults, ti.pad, dev).c_str());
                if (h.track) h.track(std::string("Herstel:") + ti.section + ":" + r.name);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
        ImGui::Spacing();
    }
    if (h.f_small) ImGui::PopFont();
    if (!map.empty()) {
        if (ImGui::Button("Alles terug naar standaard")) map.clear();
        if (h.track) h.track(std::string("Herstel alles:") + ti.section);
    }
}

} // namespace

// ------------------------------------------------------------------------------------------- public
bool ControlsCapturing() { return T().cap.active; }
void ControlsCancelCapture() {
    if (T().cap.active) EndCapture("Geannuleerd.", false);
}

void ApplyGlyphSet(const Settings& saved) {
    TabState& s = T();
    const char* env_set = std::getenv("NWII_GLYPH_SET");
    const std::string setting = env_set && *env_set ? std::string(env_set) : saved.glyph_set;
    const char* ep = std::getenv("NWII_INPUT_PRESET_GAMEPAD");
    const char* ek = std::getenv("NWII_INPUT_PRESET_KEYBOARD");
    const std::string pad = ep && *ep ? std::string(ep) : saved.preset_gamepad;
    const std::string kb = ek && *ek ? std::string(ek) : saved.preset_keyboard;
    const Device d = s.dev.known ? s.dev.last : PadConnected() ? glyphs::DeviceFromType(FirstPadType()) : Device::Keyboard;
    const std::string folder = glyphs::ResolveFolder(setting, d, pad, kb);
    if (folder == s.glyph_folder) return;
    s.glyph_folder = folder;
    runtime::hle::HdTexSetGlyphSet(folder);
    std::cout << "[CTRL18-UI] glyph set \"" << setting << "\" -> " << (folder.empty() ? "wii (original)" : folder)
              << " (device " << glyphs::DeviceName(d) << (s.dev.known ? ", last used" : ", at start") << ")" << std::endl;
}

bool ControlsEvent(const SDL_Event& e, Settings& edit, const Settings& saved) {
    TabState& s = T();
    NoteLive(e);
    const bool pad_event = e.type == SDL_CONTROLLERBUTTONDOWN || e.type == SDL_CONTROLLERAXISMOTION;
    if (s.dev.Note(e, pad_event ? FirstPadType() : "")) ApplyGlyphSet(saved); // pad type only for pad events
    if (!s.cap.active) return false;
    const ct::TableInfo ti = ct::Table(s.cap.table);
    std::string name;
    bool consumed = false;
    switch (e.type) {
    case SDL_KEYDOWN: {
        consumed = true;
        if (e.key.repeat) break;
        const SDL_Scancode sc = e.key.keysym.scancode;
        if (sc == SDL_SCANCODE_ESCAPE) { EndCapture("Geannuleerd.", false); break; }
        if (ti.pad) { s.msg = "Druk op een knop van de controller (Esc = annuleren)."; s.msg_err = true; break; }
        if (ct::ReservedKey(sc)) { s.msg = "F1, F11 en F12 zijn gereserveerd (menu, volledig scherm, schermafdruk)."; s.msg_err = true; break; }
        name = ct::KeyName(sc);
        if (name.empty()) { s.msg = "Deze toets wordt niet ondersteund; kies een andere."; s.msg_err = true; }
        break;
    }
    case SDL_KEYUP:
        consumed = true;
        break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        if (!ti.pad) {
            consumed = true;
            if (e.type == SDL_MOUSEBUTTONDOWN) name = ct::MouseName(e.button.button);
        }
        break;
    case SDL_CONTROLLERBUTTONDOWN:
        consumed = true;
        if (!ti.pad) { s.msg = "Druk op een toets of muisknop (Esc = annuleren)."; s.msg_err = true; break; }
        if (e.cbutton.button == SDL_CONTROLLER_BUTTON_MISC1) { s.msg = "Deze knop opent het menu en is gereserveerd."; s.msg_err = true; break; }
        name = ct::PadButtonName(e.cbutton.button, s.cap.table == ct::TableId::PadHd, NintendoPad());
        break;
    case SDL_CONTROLLERBUTTONUP:
        consumed = true;
        break;
    case SDL_CONTROLLERAXISMOTION:
        consumed = true;
        if (ti.pad && e.caxis.value > 20000) name = ct::TriggerName(e.caxis.axis);
        break;
    default:
        break;
    }
    if (!name.empty()) {
        Assign(edit, s.cap.table, s.cap.action, name);
        const std::string help = ActionHelp(s.cap.table, s.cap.action);
        EndCapture(ct::InputLabel(name, ti.pad, ti.pad ? PadFamily() : Device::Keyboard) + " -> " + help +
                       ". Druk op Opslaan om te bewaren.", false);
        std::cout << "[CTRL18-UI] rebind [" << ti.section << "] " << s.cap.action << " = \"" << name << "\"" << std::endl;
    }
    return consumed;
}

std::string ControlsTakeGyroHint(const Settings& saved) {
    TabState& s = T();
    if (s.gyro_hint_given || runtime::ui_config().gyro_hint_shown) return {};
    const uint32_t now = SDL_GetTicks();
    if (now - s.gyro_hint_check_t < 1000) return {};
    s.gyro_hint_check_t = now;
    if (saved.preset_gamepad != "switch-hd" || saved.gyro_aim) return {};
    if (!runtime::input::ctrl18_live_status().gyro_hint) return {};
    s.gyro_hint_given = true;
    return "Je controller heeft een gyro: richten door de controller te bewegen kan aan onder Instellingen > Besturing "
           "(\"Richten met gyro\", F1).";
}

std::string ControlsQuery(const std::string& q, const Settings& edit, const Settings& saved) {
    TabState& s = T();
    if(q=="gyro_player_space")return edit.gyro_player_space ? "1" : "0";
    if(q=="gyro_cal_capable")return runtime::input::calibration_status(s.gyro_cal_slot).capable ? "1" : "0";
    if (q == "capture") return s.cap.active ? "1" : "0";
    if (q == "capture_action") return s.cap.active ? s.cap.action : "none";
    if (q == "capture_msg") return s.msg;
    if (q == "last_device") return s.dev.known ? glyphs::DeviceName(s.dev.last) : "none";
    if (q == "glyph_folder") return s.glyph_folder == "\x01" ? "none" : s.glyph_folder.empty() ? "wii" : s.glyph_folder;
    if (q == "glyph_active") {
        const std::string a = runtime::hle::HdTexGlyphSet();
        return a.empty() ? "wii" : a;
    }
    if (q == "last_input") return s.last_input.empty() ? "none" : s.last_input;
    // bind:<section>:<action> (edit), savedbind:<section>:<action>, conflicts:<section>
    for (const char* pre : {"bind:", "savedbind:", "conflicts:"}) {
        const size_t n = std::strlen(pre);
        if (q.compare(0, n, pre) != 0) continue;
        const std::string rest = q.substr(n);
        const size_t c = rest.find(':');
        bool ok = false;
        const ct::TableId t = TableFromSection(rest.substr(0, c), ok);
        if (!ok) return "?";
        const Settings& src = std::strcmp(pre, "savedbind:") == 0 ? saved : edit;
        const std::vector<ct::Row> rows = ct::Rows(t);
        if (std::strcmp(pre, "conflicts:") == 0)
            return std::to_string(ct::Conflicts(rows, Map(src, t), ct::Table(t).pad).size());
        const std::string action = c == std::string::npos ? std::string() : rest.substr(c + 1);
        for (const ct::Row& r : rows)
            if (r.name == action) {
                std::string out;
                for (const std::string& x : ct::Effective(r, Map(src, t))) out += (out.empty() ? "" : ",") + x;
                return out.empty() ? "none" : out;
            }
        return "?";
    }
    // saved./edit. values of the controls fields (ui_app.cpp SettingValue falls back to this with "value:<id>")
    if (q.compare(0, 6, "value:") == 0 || q.compare(0, 10, "editvalue:") == 0) {
        const bool ed = q[0] == 'e';
        const Settings& v = ed ? edit : saved;
        const std::string k = q.substr(q.find(':') + 1);
        const auto b = [](bool x) { return std::string(x ? "1" : "0"); };
        if (k == "preset_gamepad") return v.preset_gamepad;
        if (k == "preset_keyboard") return v.preset_keyboard;
        if (k == "gyro_aim") return b(v.gyro_aim);
        if (k == "rumble") return std::to_string(v.rumble);
        if (k == "free_camera") return b(v.free_camera);
        if (k == "camera_speed") return std::to_string(v.camera_speed);
        if (k == "camera_invert_x") return b(v.camera_invert_x);
        if (k == "camera_invert_y") return b(v.camera_invert_y);
        if (k == "flight_invert_y") return b(v.flight_invert_y);
        if (k == "mouse_camera_sensitivity") return std::to_string(v.mouse_camera_sensitivity);
        if (k == "mouse_aim_sensitivity") return std::to_string(v.mouse_aim_sensitivity);
        if (k == "glyph_set") return v.glyph_set;
    }
    return "?";
}

void RenderGyroCalibration(Settings& e,const ControlsTabHost& h) {
    TabState& st=T();
    const bool english=runtime::ui_config().language=="en";
    const auto section=[&](const char* t){if(h.section)h.section(t);};
    const auto hint=[&](const char* t){if(h.hint)h.hint(t);};
    const auto track=[&](const std::string& t){if(h.track)h.track(t);};
    ImGui::Checkbox(english ? "Gravity-relative aiming" : Label("gyro_player_space"), &e.gyro_player_space);
    Tip("gyro_player_space");track("Mikken volgt je houding");
    section(english ? "Gyro calibration" : "Gyro kalibreren");track("Gyro kalibreren");
    runtime::input::ControllerStatus pads[4];runtime::input::controller_status(pads);
    int& cal_slot=st.gyro_cal_slot;
    for(int i=0;i<4;++i)if(pads[i].connected){
        ImGui::PushID(i);
        if(ImGui::RadioButton(pads[i].name,cal_slot==i))cal_slot=i;
        ImGui::PopID();
    }
    const auto status=runtime::input::calibration_status(cal_slot,true);
    if(!pads[cal_slot].connected || !status.capable){
        hint(english ? "Connect a controller with motion sensors. Keyboard/mouse and controllers without gyro use the virtual controls; sensor calibration is not needed."
                     : "Sluit een controller met bewegingssensoren aan. Toetsenbord/muis en controllers zonder gyro gebruiken virtuele besturing; sensorkalibratie is daarvoor niet nodig.");
    }else{
        hint(english ? "1. Put the controller on a stable surface. 2. Collect two seconds of still sensor data. 3. Apply the correction, then test aiming. This calibration lasts until you reconnect or close SkyBreak."
                     : "1. Leg de controller op een stabiele ondergrond. 2. Meet twee seconden stilstand. 3. Pas de correctie toe en test het richten. Deze kalibratie blijft tot je opnieuw aansluit of SkyBreak afsluit.");
        if(ImGui::Button(english ? "Measure stillness" : "Stilstand meten"))runtime::input::calibration_begin(cal_slot);
        track("Stilstand meten");
        if(status.sample.collecting){
            ImGui::SameLine();if(ImGui::Button(english ? "Cancel measurement" : "Meten annuleren"))runtime::input::calibration_cancel(cal_slot);
            ImGui::ProgressBar(std::clamp(status.sample.seconds/2.f,0.f,1.f));
            ImGui::TextWrapped("%s",english ? "Keep it still. Movement or missing sensor data restarts the measurement." : "Houd hem stil. Beweging of ontbrekende sensordata begint de meting opnieuw.");
        }
        if(status.sample.ready){
            ImGui::SameLine();if(ImGui::Button(english ? "Apply correction" : "Correctie toepassen"))runtime::input::calibration_apply(cal_slot);
            track("Correctie toepassen");
        }
        ImGui::Text("%s: %s",english ? "Calibration" : "Kalibratie",status.applied ? (english ? "applied" : "toegepast") : (english ? "not applied" : "niet toegepast"));
        // Live cursor is a separate preview. It never writes to the game pointer.
        static float px=0,py=0;static uint64_t last=0;static int32_t preview_id=-1;static runtime::input::GravityAim preview_aim;
        const uint64_t now=runtime::input::calibration_now();
        if(preview_id!=status.id){px=py=0;last=now;preview_id=status.id;preview_aim.reset();}
        if(last && now-last<100 && now-status.last_event_ms<100){
            const float dt=float(now-last)*.001f;
            float rates[3],accel[3],mapped[3];
            for(int a=0;a<3;++a){const float sign=a==1?1.f:-1.f;rates[a]=sign*(status.sample.latest[a]-(status.applied?status.bias[a]:0));accel[a]=-sign*status.sample.accel[a]/9.80665f;}
            preview_aim.map(rates,accel,dt,mapped);
            px=std::clamp(px-(e.gyro_player_space?mapped[1]:rates[1])*dt*2,-1.f,1.f);
            py=std::clamp(py+(e.gyro_player_space?mapped[0]:rates[0])*dt*2,-1.f,1.f);
        }last=now;
        if(ImGui::Button(english ? "Centre test pointer" : "Testwijzer centreren"))px=py=0;
        track("Testwijzer centreren");
        const ImVec2 origin=ImGui::GetCursorScreenPos(),size(240*h.s,100*h.s);
        ImGui::Dummy(size);auto* dl=ImGui::GetWindowDrawList();
        dl->AddRect(origin,ImVec2(origin.x+size.x,origin.y+size.y),IM_COL32(180,190,200,160));
        dl->AddLine(ImVec2(origin.x+size.x/2,origin.y),ImVec2(origin.x+size.x/2,origin.y+size.y),IM_COL32(180,190,200,70));
        dl->AddCircleFilled(ImVec2(origin.x+(px+1)*size.x/2,origin.y+(py+1)*size.y/2),5*h.s,IM_COL32(240,200,120,255));
        hint(english ? "Centring chooses the pointing direction; calibration corrects sensor drift. Test again after the controller warms up."
                     : "Centreren bepaalt de richtingsnul; kalibreren corrigeert sensordrift. Test opnieuw als de controller warm is geworden.");
    }

}

void RenderControlsTab(Settings& e, const Settings& o, const ControlsTabHost& h) {
    TabState& st = T();
    (void)o;
    if (st.cap.active && SDL_GetTicks() - st.cap.t0 > 10000) EndCapture("Geen knop ingedrukt; niets veranderd.", false);
    const auto section = [&](const char* t) { if (h.section) h.section(t); };
    const auto hint = [&](const char* t) { if (h.hint) h.hint(t); };
    const auto track = [&](const std::string& t) { if (h.track) h.track(t); };

    // ---- presets
    section("Besturing");
    {
        const char* pad_name = nullptr;
        const char* type = FirstPadType(&pad_name);
        if (h.f_small) ImGui::PushFont(h.f_small);
        if (*type)
            ImGui::Text("Controller: %s (%s-knoppen)  \xC2\xB7  laatst gebruikt: %s", pad_name, FamilyNl(PadFamily()),
                        st.dev.known ? FamilyNl(st.dev.last) : "-");
        else
            ImGui::TextDisabled("Geen controller verbonden: toetsenbord + muis.");
        if (h.f_small) ImGui::PopFont();
    }
    const float avail = ImGui::GetContentRegionAvail().x;
    const float sp = ImGui::GetStyle().ItemSpacing.x;
    const ImVec2 card3((avail - 2 * sp) / 3.0f, 92.0f * h.s), card2((avail - sp) / 2.0f, 74.0f * h.s);
    ImGui::TextUnformatted(Label("preset_gamepad"));
    if (PresetCard("hd", "Switch HD (knoppen)", "Zoals Skyward Sword HD: rechterstick = zwaard, ZR = voorwerp, L + stick = camera.",
                   "Aanbevolen", e.preset_gamepad == "switch-hd", card3, h))
        e.preset_gamepad = "switch-hd";
    Tip("preset_gamepad");
    ImGui::SameLine();
    if (PresetCard("classic", "Klassiek", "De indeling van voor deze versie (stick-zwaard, A onderaan).", "",
                   e.preset_gamepad == "classic", card3, h))
        e.preset_gamepad = "classic";
    ImGui::SameLine();
    if (PresetCard("wii", "Wii (beweging/gyro)", "Zwaaien met een DualSense of Switch Pro; rechterstick = camera.", "",
                   e.preset_gamepad == "wii-motion", card3, h))
        e.preset_gamepad = "wii-motion";
    ImGui::TextUnformatted(Label("preset_keyboard"));
    if (PresetCard("kbm", "Toetsenbord + muis", "WASD, muis = camera, linkermuisknop = zwaard, E = actie, Esc = uitrusting.",
                   "", e.preset_keyboard == "kbm", card2, h))
        e.preset_keyboard = "kbm";
    Tip("preset_keyboard");
    ImGui::SameLine();
    if (PresetCard("kb_classic", "Klassiek", "Muis = Wii-wijzer, numeriek toetsenblok = zwaardslagen.", "",
                   e.preset_keyboard == "classic", card2, h))
        e.preset_keyboard = "classic";
    hint("Knoppen op een PlayStation- of Xbox-controller zitten op dezelfde plek als op een Switch: de rechter knop "
         "(Cirkel / B) is A, de onderste (Kruis / A) is B.");

    // ---- options
    ImGui::Spacing();
    section("Camera en gevoel");
    ImGui::Checkbox(Label("free_camera"), &e.free_camera);
    Tip("free_camera");
    track("Vrije camera");
    if (e.preset_gamepad == "wii-motion") {
        ImGui::SameLine();
        ImGui::TextDisabled("(niet in de Wii-indeling met de controller)");
    }
    static const char* kSpeed[] = {"", "1 (rustig)", "2", "3 (standaard)", "4", "5 (snel)"};
    int sp5 = std::clamp(e.camera_speed, 1, 5);
    if (ImGui::SliderInt(Label("camera_speed"), &sp5, 1, 5, kSpeed[sp5])) e.camera_speed = sp5;
    Tip("camera_speed");
    track("Camerasnelheid");
    ImGui::Checkbox(Label("camera_invert_x"), &e.camera_invert_x);
    Tip("camera_invert_x");
    track("Camera horizontaal omkeren");
    ImGui::Checkbox(Label("camera_invert_y"), &e.camera_invert_y);
    Tip("camera_invert_y");
    track("Camera verticaal omkeren");
    ImGui::Checkbox(Label("flight_invert_y"), &e.flight_invert_y);
    Tip("flight_invert_y");
    track("Vliegen omkeren");
    ImGui::SliderFloat(Label("mouse_camera_sensitivity"), &e.mouse_camera_sensitivity, 0.1f, 5.0f, "%.2f",
                       ImGuiSliderFlags_Logarithmic);
    Tip("mouse_camera_sensitivity");
    ImGui::SliderFloat(Label("mouse_aim_sensitivity"), &e.mouse_aim_sensitivity, 0.1f, 5.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
    Tip("mouse_aim_sensitivity");
    int rumble = std::clamp(e.rumble, 0, 100);
    if (ImGui::SliderInt(Label("rumble"), &rumble, 0, 100, rumble == 0 ? "uit" : "%d%%")) e.rumble = rumble;
    Tip("rumble");
    track("Trillen");
    ImGui::Checkbox(Label("gyro_aim"), &e.gyro_aim);
    Tip("gyro_aim");
    track("Richten met gyro");
    {
        static const char* kVal[] = {"auto", "ps", "xbox", "switch", "kbm", "wii"};
        static const char* kNl[] = {"Automatisch (laatst gebruikte apparaat)", "PlayStation", "Xbox", "Nintendo Switch",
                                    "Toetsenbord + muis", "Wii (origineel)"};
        int gi = -1;
        for (int i = 0; i < 6; ++i)
            if (e.glyph_set == kVal[i]) gi = i;
        const std::string preview = gi >= 0 ? kNl[gi] : e.glyph_set;
        if (ImGui::BeginCombo(Label("glyph_set"), preview.c_str())) {
            for (int i = 0; i < 6; ++i) {
                if (ImGui::Selectable(kNl[i], gi == i)) e.glyph_set = kVal[i];
                track(std::string("Knopiconen/") + kNl[i]);
            }
            ImGui::EndCombo();
        }
        Tip("glyph_set");
        track("Knopiconen");
        const std::string active = runtime::hle::HdTexGlyphSet();
        if (h.f_small) ImGui::PushFont(h.f_small);
        ImGui::TextDisabled("Nu in beeld: %s", active.empty() ? (h.in_game ? "Wii (origineel)" : "(na het starten)") : active.c_str());
        if (h.f_small) ImGui::PopFont();
    }

    RenderGyroCalibration(e,h);

    // ---- rebinding
    ImGui::Spacing();
    section("Knoppen en toetsen");
    int view = st.bind_view >= 0 ? st.bind_view : (st.dev.known && st.dev.last == Device::Keyboard) || !PadConnected() ? 1 : 0;
    if (ImGui::RadioButton("Controller", view == 0)) st.bind_view = view = 0;
    track("Toewijzen controller");
    ImGui::SameLine();
    if (ImGui::RadioButton("Toetsenbord + muis", view == 1)) st.bind_view = view = 1;
    track("Toewijzen toetsenbord");
    const ct::TableId table = view == 0 ? ct::PadTable(e.preset_gamepad) : ct::KeyTable(e.preset_keyboard);
    if (h.f_small) ImGui::PushFont(h.f_small);
    ImGui::TextDisabled("Indeling: %s  \xC2\xB7  [%s]  \xC2\xB7  Wijzig = druk daarna de nieuwe knop/toets, Esc = annuleren.",
                        view == 0 ? (e.preset_gamepad == "switch-hd" ? "Switch HD (knoppen)" : e.preset_gamepad == "classic" ? "Klassiek" : "Wii (beweging/gyro)")
                                  : (e.preset_keyboard == "kbm" ? "Toetsenbord + muis" : "Klassiek"),
                        ct::Table(table).section);
    if (h.f_small) ImGui::PopFont();
    if (!st.msg.empty()) ImGui::TextColored(st.msg_err ? ImVec4(1.0f, 0.55f, 0.35f, 1.0f) : ImVec4(0.75f, 0.9f, 0.6f, 1.0f), "%s", st.msg.c_str());
    BindingTable(e, table, h);

    // ---- live test panel
    ImGui::Spacing();
    if (h.selftest) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    const bool test_open = ImGui::CollapsingHeader("Test je besturing", ImGuiTreeNodeFlags_DefaultOpen);
    track("Test je besturing");
    if (test_open) {
        StickWidget("L", st.axis[SDL_CONTROLLER_AXIS_LEFTX], st.axis[SDL_CONTROLLER_AXIS_LEFTY], h.s);
        ImGui::SameLine();
        StickWidget("R", st.axis[SDL_CONTROLLER_AXIS_RIGHTX], st.axis[SDL_CONTROLLER_AXIS_RIGHTY], h.s);
        ImGui::SameLine();
        TriggerWidget(PadFamily() == Device::PlayStation ? "L2" : PadFamily() == Device::Switch ? "ZL" : "LT",
                      st.axis[SDL_CONTROLLER_AXIS_TRIGGERLEFT], h.s);
        ImGui::SameLine();
        TriggerWidget(PadFamily() == Device::PlayStation ? "R2" : PadFamily() == Device::Switch ? "ZR" : "RT",
                      st.axis[SDL_CONTROLLER_AXIS_TRIGGERRIGHT], h.s);
        ImGui::SameLine();
        ImGui::BeginGroup();
        if (h.f_small) ImGui::PushFont(h.f_small);
        std::string held;
        for (int b = 0; b < 21; ++b)
            if (st.pad_held & (1u << b)) {
                const std::string n = ct::PadButtonName(b, false, NintendoPad());
                if (!n.empty()) held += (held.empty() ? "" : ", ") + ct::PadLabel(n, PadFamily());
            }
        ImGui::Text("Ingedrukt: %s", held.empty() ? "-" : held.c_str());
        ImGui::Text("Laatste invoer: %s%s%s", st.last_input.empty() ? "-" : st.last_input.c_str(),
                    st.last_device.empty() ? "" : "  \xC2\xB7  ", st.last_device.c_str());
        if (h.in_game) {
            const runtime::input::Ctrl18Status ls = runtime::input::ctrl18_live_status();
            ImGui::Text("Indeling in het spel: %s", *ls.preset ? ls.preset : "-");
            ImGui::Text("Situatie van Link: %s", *ls.situation ? ls.situation : "-");
            ImGui::Text("Herkende zwaardbeweging: %s%s", *ls.last_move ? ls.last_move : "-",
                        ls.moves ? (" (" + std::to_string(ls.moves) + ")").c_str() : "");
        } else {
            ImGui::TextDisabled("Herkende zwaardbeweging en Links situatie: in het spel (F1).");
        }
        if (h.f_small) ImGui::PopFont();
        ImGui::EndGroup();
        hint("In het spel (F1) zie je hier welke zwaardbeweging de rechterstick of muis maakt; het spel krijgt "
             "geen invoer zolang het menu open is.");
    }
}

} // namespace nwii::ui
