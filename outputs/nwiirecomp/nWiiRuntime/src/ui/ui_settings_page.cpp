// LAUNCH19: the SkyBreak settings page (see ui_settings_page.h). Layout ported from the LAUNCH19 mockup tool
// (work/launcher19/mockup/mockup.cpp, which drew the same catalog on the CPU), now live: input, dialogs, sub page.
#include "ui_settings_page.h"
#include "../input/pad_hub.h"
#include "input/controller_events.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "imgui_internal.h"

namespace nwii::ui::page {

namespace cat = catalog;

namespace {

// ------------------------------------------------------------------------------------------------ looks
const ImU32 kParch = IM_COL32(244, 232, 205, 255);
const ImU32 kMuted = IM_COL32(190, 176, 146, 255);
const ImU32 kDim = IM_COL32(150, 138, 112, 255);
const ImU32 kOrange = IM_COL32(242, 168, 90, 255);
const ImU32 kGreen = IM_COL32(140, 214, 130, 255);
const ImU32 kBlue = IM_COL32(130, 186, 236, 255);
const ImU32 kRed = IM_COL32(240, 96, 80, 255);

inline ImU32 A(ImU32 c, float a) { return (c & 0x00FFFFFF) | (ImU32(std::clamp(a, 0.0f, 1.0f) * ((c >> 24) & 0xFF)) << 24); }

struct Ctx { // per-frame drawing context
    ImDrawList* dl = nullptr;
    const Host* h = nullptr;
    float s = 1.0f;
    ImU32 acc = 0, acc_hi = 0;
    float P(float v) const { return v * s; }
    const char* L(const char* nl, const char* en) const { return h->en ? en : nl; }
};

ImVec2 TextSize(ImFont* f, const char* t, float size = 0, float wrap = 0) {
    return f->CalcTextSizeA(size > 0 ? size : f->FontSize, FLT_MAX, wrap, t);
}
void Text(ImDrawList* dl, ImFont* f, ImVec2 p, ImU32 c, const char* t, float size = 0) {
    dl->AddText(f, size > 0 ? size : f->FontSize, p, c, t);
}
void TextRight(ImDrawList* dl, ImFont* f, float right_x, float y, ImU32 c, const char* t) {
    Text(dl, f, ImVec2(right_x - TextSize(f, t).x, y), c, t);
}
float Wrapped(ImDrawList* dl, ImFont* f, ImVec2 p, float w, ImU32 c, const char* t) {
    dl->AddText(f, f->FontSize, p, c, t, nullptr, w);
    return TextSize(f, t, 0, w).y;
}
void Chevron(ImDrawList* dl, ImVec2 c, float r, bool right, ImU32 col) {
    const float d = right ? 1.0f : -1.0f;
    dl->AddTriangleFilled(ImVec2(c.x + d * r, c.y), ImVec2(c.x - d * r * 0.6f, c.y - r), ImVec2(c.x - d * r * 0.6f, c.y + r), col);
}
void Check(const Ctx& x, ImVec2 c, float r, ImU32 col) {
    x.dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x - r * 0.3f, c.y + r * 0.7f), col, x.P(2.2f));
    x.dl->AddLine(ImVec2(c.x - r * 0.3f, c.y + r * 0.7f), ImVec2(c.x + r, c.y - r * 0.8f), col, x.P(2.2f));
}
void Cross(const Ctx& x, ImVec2 c, float r, ImU32 col) {
    x.dl->AddLine(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), col, x.P(2.2f));
    x.dl->AddLine(ImVec2(c.x - r, c.y + r), ImVec2(c.x + r, c.y - r), col, x.P(2.2f));
}
void RestartIcon(const Ctx& x, ImVec2 c, float r, ImU32 col) {
    x.dl->PathArcTo(c, r, 0.6f, 5.6f, 16);
    x.dl->PathStroke(col, 0, x.P(1.6f));
    const ImVec2 tip(c.x + r * std::cos(0.6f), c.y + r * std::sin(0.6f));
    x.dl->AddTriangleFilled(ImVec2(tip.x + x.P(3.5f), tip.y - x.P(1)), ImVec2(tip.x - x.P(3), tip.y - x.P(2.5f)),
                            ImVec2(tip.x, tip.y + x.P(3.5f)), col);
}
void FolderIcon(const Ctx& x, ImVec2 p, float w, ImU32 col) {
    const float h = w * 0.72f;
    x.dl->AddRectFilled(ImVec2(p.x, p.y), ImVec2(p.x + w * 0.45f, p.y + h * 0.3f), col, x.P(1.5f));
    x.dl->AddRectFilled(ImVec2(p.x, p.y + h * 0.18f), ImVec2(p.x + w, p.y + h), col, x.P(2));
}
// A pill-shaped label (description tags, restart chip); returns its width.
float Chip(const Ctx& x, ImVec2 p, const char* t, ImU32 col, bool icon_restart = false, ImFont* f = nullptr) {
    if (!f) f = x.h->f.small;
    const ImVec2 ts = TextSize(f, t);
    const float ic = icon_restart ? x.P(16) : 0;
    const float w = ts.x + x.P(16) + ic, hh = ts.y + x.P(4);
    x.dl->AddRectFilled(p, ImVec2(p.x + w, p.y + hh), A(col, 0.16f), hh * 0.5f);
    x.dl->AddRect(p, ImVec2(p.x + w, p.y + hh), A(col, 0.65f), hh * 0.5f, 0, x.P(1));
    if (icon_restart) RestartIcon(x, ImVec2(p.x + x.P(12), p.y + hh * 0.5f), x.P(4.5f), col);
    Text(x.dl, f, ImVec2(p.x + x.P(8) + ic, p.y + x.P(2)), col, t);
    return w;
}
float ChipWidth(const Ctx& x, const char* t, bool icon_restart = false) {
    return TextSize(x.h->f.small, t).x + x.P(16) + (icon_restart ? x.P(16) : 0);
}

bool Hover(ImVec2 a, ImVec2 b) { return ImGui::IsMouseHoveringRect(a, b, false); }
bool MouseMoved() {
    const ImGuiIO& io = ImGui::GetIO();
    return io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f;
}
bool Clicked(ImVec2 a, ImVec2 b, int button = 0) { return Hover(a, b) && ImGui::IsMouseClicked(button); }

// ------------------------------------------------------------------------------------------------ values
// The choices of a row; [audio] device also offers the host's device names.
struct ChoiceList {
    std::vector<std::string> lits;   // TOML literal per choice (string rows: the quoted string)
    std::vector<std::string> labels; // NL or EN
};
ChoiceList Choices(const cat::Desc& d, const Host& h) {
    ChoiceList c;
    for (const cat::Choice& ch : d.choices) {
        c.lits.push_back(ch.toml ? ch.toml : "");
        c.labels.push_back(cat::T(ch.label, h.en));
    }
    if (!std::strcmp(d.id, "audio_device"))
        for (const std::string& n : h.audio_devices) {
            c.lits.push_back(cat::QuoteToml(n));
            c.labels.push_back(n);
        }
    return c;
}
int CurrentChoice(const cat::Desc& d, const Host& h, const Settings& s) {
    const int i = cat::ChoiceIndex(d, s);
    if (i >= 0) return i;
    if (!std::strcmp(d.id, "audio_device"))
        for (size_t k = 0; k < h.audio_devices.size(); ++k)
            if (s.audio_device == h.audio_devices[k]) return int(d.choices.size() + k);
    return -1;
}
void SetChoiceAt(const cat::Desc& d, const Host& h, Settings& s, int i) {
    if (i < int(d.choices.size())) {
        cat::SetChoice(d, s, i);
        return;
    }
    const size_t k = size_t(i) - d.choices.size();
    if (!std::strcmp(d.id, "audio_device") && k < h.audio_devices.size()) s.audio_device = h.audio_devices[k];
}

float FloatValue(const cat::Desc& d, const Settings& s) {
    const cat::Ref r = cat::RefOf(d, s);
    if (auto* i = std::get_if<int*>(&r)) return float(**i);
    if (auto* f = std::get_if<float*>(&r)) {
        if (**f >= 0 || (d.def && *d.def)) return **f;
        if (!std::strcmp(d.id, "left_deadzone") || !std::strcmp(d.id, "right_deadzone")) return kDefaultStickDeadzone;
        return (d.min + d.max) * 0.5f; // built-in value of an absent key: start from the middle
    }
    return 0.0f;
}
std::string Shorten(const std::string& p, size_t n) {
    if (p.size() <= n) return p;
    return "\xE2\x80\xA6" + p.substr(p.size() - (n - 1));
}

std::string ValueText(const cat::Desc& d, const Host& h, const Settings& s) {
    const bool en = h.en;
    if (d.kind == cat::Kind::Info) {
        if (!std::strcmp(d.id, "renderer")) return h.renderer.empty() ? "OpenGL" : h.renderer;
        if (!std::strcmp(d.id, "save_type")) return en ? "Wii NAND folder (Dolphin layout)" : "Wii-NAND-map (Dolphin-indeling)";
        if (!std::strcmp(d.id, "settings_file")) return Shorten(h.settings_path, 40);
        if (!std::strcmp(d.id, "game_dir")) {
            if (!h.disc_done) return en ? "checking..." : "controleren...";
            return h.disc_ok ? "SOUE01  \xC2\xB7  USA 1.00" : (en ? "not usable" : "klopt niet");
        }
        return "";
    }
    if (!d.ref) return "";
    if (d.kind == cat::Kind::Choice) {
        const ChoiceList c = Choices(d, h);
        const int i = CurrentChoice(d, h, s);
        if (i >= 0 && i < int(c.labels.size())) return c.labels[size_t(i)];
        const cat::Ref r = cat::RefOf(d, s);
        if (auto* p = std::get_if<std::string*>(&r)) return **p; // a hand-written value outside the list
        if (auto* p = std::get_if<int*>(&r)) return std::to_string(**p);
        return "?";
    }
    const cat::Ref r = cat::RefOf(d, s);
    char b[96];
    if (auto* p = std::get_if<bool*>(&r)) return **p ? (en ? "On" : "Aan") : (en ? "Off" : "Uit");
    if (auto* p = std::get_if<int*>(&r)) {
        std::snprintf(b, sizeof b, d.fmt ? d.fmt : "%d", **p);
        return b;
    }
    if (auto* p = std::get_if<float*>(&r)) {
        if (**p < 0 && d.def && !*d.def) {
            std::snprintf(b, sizeof b, "%s (%.2f)", en ? "Default" : "Standaard", FloatValue(d, s));
        } else {
            float v = **p;
            if (d.flags & cat::kPercent) v *= 100.0f;
            std::snprintf(b, sizeof b, d.fmt ? d.fmt : "%.2f", v);
        }
        std::string o = b;
        if (!en) std::replace(o.begin(), o.end(), '.', ',');
        if (en) { // "graden" in the grip format
            const size_t g = o.find(" graden");
            if (g != std::string::npos) o.replace(g, 7, " degrees");
        }
        return o;
    }
    if (auto* p = std::get_if<std::string*>(&r)) return **p;
    return "";
}

std::string DefaultText(const cat::Desc& d, const Host& h) {
    std::string def = d.def ? d.def : "";
    for (const cat::Choice& c : d.choices)
        if (c.toml && def == c.toml) return cat::T(c.label, h.en);
    if (d.kind == cat::Kind::Toggle) return def == "true" ? (h.en ? "On" : "Aan") : (h.en ? "Off" : "Uit");
    def = cat::Unquote(def);
    if (def.empty()) return h.en ? "automatic" : "automatisch";
    if (d.kind == cat::Kind::Slider) {
        Settings tmp;
        cat::ApplyDefault(d, tmp);
        return ValueText(d, h, tmp);
    }
    return def;
}

bool Differs(const cat::Desc& d, const Settings& a, const Settings& b) {
    return cat::IsValueRow(d) && !cat::SameValue(d, a, b);
}
bool BindsDiffer(const Settings& a, const Settings& b) {
    return a.bind_pad_hd != b.bind_pad_hd || a.bind_pad_classic != b.bind_pad_classic || a.bind_kbm != b.bind_kbm ||
           a.bind_kb_classic != b.bind_kb_classic;
}

cat::Avail AvailOf(const cat::Desc& d, const Host& h) {
    cat::Avail av = d.avail ? d.avail(*h.edit, h.ctx) : cat::Avail{};
    if (av.enabled && d.env && h.ctx.env) { // every row with an env override is locked while it is set
        const cat::Avail l = cat::Locked(h.ctx, d.env);
        if (!l.enabled) av = l;
    }
    if (!std::strcmp(d.id, "pause_unfocused") && av.enabled) // the key is stored; the guest pause needs a proof run first
        av = {true, false,
              {"Nog niet actief in deze versie: het spel stilzetten moet eerst in een proefrun bewijzen dat geluid, "
               "klok en de bevriezingsdetector het overleven. De keuze wordt wel al bewaard (standaard uit).",
               "Not active in this version yet: pausing the game needs a proof run first. The choice is stored (off)."}};
    if (!std::strcmp(d.id, "settings_file") && !h.settings_problem.empty())
        av = {true, true, {h.settings_problem.c_str(), h.settings_problem.c_str()}};
    return av;
}

// ------------------------------------------------------------------------------------------------ rows
struct Row {
    const cat::Desc* d;
    cat::Text group;
};
std::vector<Row> RowsOf(const Host& h, int tab) {
    std::vector<Row> v, borrowed;
    const cat::Tab t = cat::Tab(tab);
    for (const cat::Desc& d : cat::All()) {
        if (d.tab != t && d.also != t) continue;
        if (h.in_game && (d.flags & cat::kLauncherOnly)) continue;
        if (!AvailOf(d, h).visible) continue;
        if (d.tab == t) v.push_back({&d, d.group});
        else borrowed.push_back({&d, d.also_group.nl ? d.also_group : d.group});
    }
    for (const Row& r : borrowed) {
        size_t at = v.size();
        for (size_t i = 0; i < v.size(); ++i)
            if (!std::strcmp(v[i].group.nl, r.group.nl)) at = i + 1;
        v.insert(v.begin() + long(at), r);
    }
    return v;
}
float RowHeight(const Ctx& x, const cat::Desc& d) { return !std::strcmp(d.id, "act_play") ? x.P(64) : x.P(44); }

const char* ApplyLabel(cat::Apply a, bool in_game, bool en) {
    switch (a) {
    case cat::Apply::Live: return en ? "Applies at once" : "Werkt meteen";
    case cat::Apply::AtBoot:
        return in_game ? (en ? "After a restart" : "Na een herstart") : (en ? "When the game starts" : "Bij de start van het spel");
    case cat::Apply::Restart: return en ? "After restarting SkyBreak" : "Na herstart van SkyBreak";
    default: return nullptr;
    }
}

} // namespace

// ================================================================================================ public helpers
ImU32 AccentHi(ImU32 c) {
    const int r = (c >> IM_COL32_R_SHIFT) & 255, g = (c >> IM_COL32_G_SHIFT) & 255, b = (c >> IM_COL32_B_SHIFT) & 255;
    return IM_COL32(std::min(255, r + (255 - r) / 2), std::min(255, g + (255 - g) / 3), std::min(255, b + (255 - b) / 4), 255);
}

std::string RawValue(const cat::Desc& d, const Settings& s) {
    const cat::Ref r = cat::RefOf(d, s);
    if (auto* p = std::get_if<bool*>(&r)) return **p ? "1" : "0";
    if (auto* p = std::get_if<int*>(&r)) return std::to_string(**p);
    if (auto* p = std::get_if<float*>(&r)) return std::to_string(**p);
    if (auto* p = std::get_if<std::string*>(&r)) return **p;
    return "?";
}

bool NeedsRestartChip(const cat::Desc& d, const Host& h) {
    if (!h.boot || !cat::IsValueRow(d)) return false;
    if (d.apply == cat::Apply::Restart) return !cat::SameValue(d, *h.edit, h.boot_app ? *h.boot_app : *h.boot);
    if (d.apply == cat::Apply::AtBoot && h.in_game) return !cat::SameValue(d, *h.edit, *h.boot);
    return false;
}
int DirtyCount(const Host& h) {
    int n = 0;
    for (const cat::Desc& d : cat::All()) n += Differs(d, *h.edit, *h.saved) ? 1 : 0;
    if (BindsDiffer(*h.edit, *h.saved)) ++n;
    if (h.edit->background != h.saved->background) ++n;
    return n;
}
int RestartCount(const Host& h) {
    int n = 0;
    for (const cat::Desc& d : cat::All()) n += NeedsRestartChip(d, h) ? 1 : 0;
    return n;
}
int ConfirmPos(const Host& h) { return h.confirm_south ? 0 : 1; }
int BackPos(const Host& h) { return h.confirm_south ? 1 : 0; }

void DrawPadGlyph(ImDrawList* dl, ImFont* f, ImVec2 c, float r, Pad pad, int pos) {
    const float lw = std::max(1.0f, r * 0.16f);
    if (pad == Pad::PlayStation) {
        static const ImU32 col[4] = {IM_COL32(124, 170, 255, 255), IM_COL32(255, 110, 110, 255),
                                     IM_COL32(236, 140, 220, 255), IM_COL32(96, 220, 190, 255)};
        dl->AddCircleFilled(c, r, IM_COL32(28, 26, 32, 235));
        dl->AddCircle(c, r, IM_COL32(200, 200, 210, 140), 0, lw * 0.6f);
        const float q = r * 0.48f;
        switch (pos) {
        case 0: // cross
            dl->AddLine(ImVec2(c.x - q, c.y - q), ImVec2(c.x + q, c.y + q), col[0], lw);
            dl->AddLine(ImVec2(c.x - q, c.y + q), ImVec2(c.x + q, c.y - q), col[0], lw);
            break;
        case 1: dl->AddCircle(c, q, col[1], 0, lw); break;                                    // circle
        case 2: dl->AddRect(ImVec2(c.x - q, c.y - q), ImVec2(c.x + q, c.y + q), col[2], 0, 0, lw); break; // square
        default:                                                                                 // triangle
            dl->AddTriangle(ImVec2(c.x, c.y - q * 1.1f), ImVec2(c.x + q, c.y + q * 0.75f), ImVec2(c.x - q, c.y + q * 0.75f),
                            col[3], lw);
        }
        return;
    }
    // Xbox (A south, B east, X west, Y north) and Switch (B south, A east, Y west, X north): letter discs
    static const char* xbox[4] = {"A", "B", "X", "Y"};
    static const char* nin[4] = {"B", "A", "Y", "X"};
    static const ImU32 xcol[4] = {IM_COL32(110, 200, 90, 255), IM_COL32(235, 80, 70, 255), IM_COL32(80, 140, 240, 255),
                                  IM_COL32(240, 200, 60, 255)};
    const bool sw = pad == Pad::Switch;
    const char* t = sw ? nin[pos & 3] : xbox[pos & 3];
    dl->AddCircleFilled(c, r, sw ? IM_COL32(52, 52, 56, 240) : IM_COL32(24, 24, 26, 240));
    dl->AddCircle(c, r, sw ? IM_COL32(220, 220, 220, 150) : A(xcol[pos & 3], 0.9f), 0, lw * 0.8f);
    if (f) {
        const float fs = r * 1.25f;
        const ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0, t);
        dl->AddText(f, fs, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), sw ? IM_COL32(240, 240, 240, 255) : xcol[pos & 3], t);
    }
}

namespace {
// A key cap ("Enter", "Esc", "Q") or a small pill ("L1", "R1", "Options"); returns its width.
float KeyCap(ImDrawList* dl, ImFont* f, ImVec2 p, float hgt, const char* t, ImU32 col = IM_COL32(236, 228, 210, 255)) {
    const float fs = hgt * 0.62f;
    const ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0, t);
    const float w = std::max(hgt, ts.x + hgt * 0.6f);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + hgt), IM_COL32(40, 34, 26, 235), hgt * 0.22f);
    dl->AddRect(p, ImVec2(p.x + w, p.y + hgt), IM_COL32(200, 186, 150, 170), hgt * 0.22f, 0, std::max(1.0f, hgt * 0.05f));
    dl->AddText(f, fs, ImVec2(p.x + (w - ts.x) * 0.5f, p.y + (hgt - ts.y) * 0.5f), col, t);
    return w;
}
} // namespace

float DrawHint(ImDrawList* dl, const Host& h, ImVec2 p, float hgt, int position, const char* key, const char* label) {
    float x = p.x;
    if (h.pad == Pad::Keyboard || position < 0) {
        if (key) x += KeyCap(dl, h.f.small, ImVec2(x, p.y), hgt, key) + hgt * 0.15f;
    } else {
        DrawPadGlyph(dl, h.f.bold, ImVec2(x + hgt * 0.5f, p.y + hgt * 0.5f), hgt * 0.5f, h.pad, position);
        x += hgt + hgt * 0.15f;
    }
    Text(dl, h.f.small, ImVec2(x + hgt * 0.1f, p.y + (hgt - h.f.small->FontSize) * 0.5f), kParch, label);
    x += TextSize(h.f.small, label).x + hgt * 0.1f;
    return x - p.x;
}

float HintWidth(const Host& h, float hgt, int position, const char* key, const char* label) {
    float w = 0;
    if (h.pad == Pad::Keyboard || position < 0) {
        if (key) w += std::max(hgt, h.f.small->CalcTextSizeA(hgt * 0.62f, FLT_MAX, 0, key).x + hgt * 0.6f) + hgt * 0.15f;
    } else {
        w += hgt + hgt * 0.15f;
    }
    return w + hgt * 0.1f + TextSize(h.f.small, label).x + hgt * 0.1f;
}

void DrawBrand(ImDrawList* dl, const Host& h, ImVec2 p, float wing_h, float word_size, bool tagline) {
    float x = p.x;
    if (h.wing && h.wing_sz.y > 0) {
        const float w = wing_h * h.wing_sz.x / h.wing_sz.y;
        dl->AddImage(h.wing, ImVec2(x, p.y), ImVec2(x + w, p.y + wing_h));
        x += w + 10 * h.s;
    }
    ImFont* word = h.f.word ? h.f.word : h.f.menu;
    const float ty = p.y + (wing_h - word_size) * 0.5f - (tagline ? 8 * h.s : 0);
    dl->AddText(word, word_size, ImVec2(x + 1.5f * h.s, ty + 2 * h.s), IM_COL32(20, 10, 4, 200), "SkyBreak");
    dl->AddText(word, word_size, ImVec2(x, ty), AccentHi(h.accent), "SkyBreak");
    if (tagline) { // the honest line (user decision), in two lines under the wordmark
        const float ly = ty + word_size + 2 * h.s;
        dl->AddText(h.f.caps, h.f.caps->FontSize, ImVec2(x + 2 * h.s, ly), kMuted,
                    h.en ? "PC PORT OF THE LEGEND OF ZELDA: SKYWARD SWORD  \xC2\xB7  BUILT FROM YOUR OWN DISC"
                         : "PC-PORT VAN THE LEGEND OF ZELDA: SKYWARD SWORD  \xC2\xB7  GEBOUWD VAN JE EIGEN SCHIJF");
        dl->AddText(h.f.caps, h.f.caps->FontSize, ImVec2(x + 2 * h.s, ly + h.f.caps->FontSize + 3 * h.s), A(kMuted, 0.8f),
                    h.en ? "NOT AN OFFICIAL NINTENDO PRODUCT" : "GEEN OFFICIEEL NINTENDO-PRODUCT");
    }
}

void State::Open(bool keep_tab) {
    if (!keep_tab) tab = 0;
    sub_controls = false;
    dialog = Dialog::None;
    pending = 0;
    dragging = false;
    scroll_to_focus = true;
    status.clear();
}

// ================================================================================================ the page
namespace {

struct Keys { // this frame's page input (keyboard + controller by position)
    bool up = false, down = false, left = false, right = false, confirm = false, back = false, def = false,
         save = false, tab_prev = false, tab_next = false, page_up = false, page_down = false, home = false,
         end = false, play = false, reset_tab = false, shift = false;
};

bool KP(ImGuiKey k, bool repeat = true) { return ImGui::IsKeyPressed(k, repeat); }

Keys ReadKeys(const Host& h) {
    const ImGuiIO& io = ImGui::GetIO();
    Keys k;
    const ImGuiKey confirm_pad = h.confirm_south ? ImGuiKey_GamepadFaceDown : ImGuiKey_GamepadFaceRight;
    const ImGuiKey back_pad = h.confirm_south ? ImGuiKey_GamepadFaceRight : ImGuiKey_GamepadFaceDown;
    k.up = KP(ImGuiKey_UpArrow) || KP(ImGuiKey_GamepadDpadUp) || KP(ImGuiKey_GamepadLStickUp);
    k.down = KP(ImGuiKey_DownArrow) || KP(ImGuiKey_GamepadDpadDown) || KP(ImGuiKey_GamepadLStickDown);
    k.left = KP(ImGuiKey_LeftArrow) || KP(ImGuiKey_GamepadDpadLeft) || KP(ImGuiKey_GamepadLStickLeft);
    k.right = KP(ImGuiKey_RightArrow) || KP(ImGuiKey_GamepadDpadRight) || KP(ImGuiKey_GamepadLStickRight);
    k.confirm = KP(ImGuiKey_Enter, false) || KP(ImGuiKey_KeypadEnter, false) || KP(ImGuiKey_Space, false) ||
                KP(confirm_pad, false);
    k.back = KP(ImGuiKey_Escape, false) || KP(ImGuiKey_Backspace, false) || KP(back_pad, false);
    k.def = (KP(ImGuiKey_Delete, false) && !io.KeyCtrl) || KP(ImGuiKey_GamepadFaceLeft, false);
    k.save = (io.KeyCtrl && KP(ImGuiKey_S, false)) || KP(ImGuiKey_GamepadFaceUp, false);
    k.tab_prev = (!io.KeyCtrl && KP(ImGuiKey_Q, false)) || KP(ImGuiKey_GamepadL1, false);
    k.tab_next = (!io.KeyCtrl && KP(ImGuiKey_E, false)) || KP(ImGuiKey_GamepadR1, false);
    k.page_up = KP(ImGuiKey_PageUp) || KP(ImGuiKey_GamepadL2);
    k.page_down = KP(ImGuiKey_PageDown) || KP(ImGuiKey_GamepadR2);
    k.home = KP(ImGuiKey_Home, false);
    k.end = KP(ImGuiKey_End, false);
    k.play = KP(ImGuiKey_F5, false) || KP(ImGuiKey_GamepadStart, false);
    k.reset_tab = (io.KeyCtrl && KP(ImGuiKey_Delete, false)) || KP(ImGuiKey_GamepadBack, false);
    k.shift = io.KeyShift;
    return k;
}

struct Page {
    State& st;
    Host& h;
    Ctx x;
    ImVec2 a, b;
    float W = 0, H = 0;
    Result result = Result::None;
    bool changed = false; // a value changed this frame (preview)
    bool dialog_fresh = false; // the dialog opened this frame: the press/click that opened it must not also answer it

    Page(State& s, Host& host, ImVec2 a0, ImVec2 b0) : st(s), h(host), a(a0), b(b0) {
        x.h = &h;
        x.s = h.s;
        x.acc = h.accent;
        x.acc_hi = AccentHi(h.accent);
        W = b.x - a.x;
        H = b.y - a.y;
    }
    float P(float v) const { return v * x.s; }
    const char* L(const char* nl, const char* en) const { return h.en ? en : nl; }
    void Track(const std::string& l, ImVec2 p0, ImVec2 p1) const {
        if (h.track) h.track(l, p0, p1);
    }
    void SetStatus(const std::string& s, bool err) {
        st.status = s;
        st.status_error = err;
        st.status_t = h.now;
    }

    // ---- actions
    void Changed() {
        changed = true;
        if (h.preview) h.preview(*h.edit);
    }
    void ChangeValue(const cat::Desc& d, int dir, bool coarse) {
        if (!AvailOf(d, h).enabled || !d.ref) return;
        Settings& e = *h.edit;
        if (d.kind == cat::Kind::Toggle) {
            const cat::Ref r = d.ref(e);
            bool* p = *std::get_if<bool*>(&r);
            const bool want = dir == 0 ? !*p : dir > 0;
            if (want == *p && dir != 0) return;
            if (!std::strcmp(d.id, "overlay") && !want && (d.flags & cat::kDanger)) { // F1 menu off: ask first
                st.danger_id = d.id;
                st.dialog = Dialog::Danger;
                st.dlg_sel = 1;
                return;
            }
            *p = want;
            Changed();
        } else if (d.kind == cat::Kind::Choice) {
            const int n = int(Choices(d, h).lits.size());
            if (n <= 0) return;
            int i = CurrentChoice(d, h, e);
            i = i < 0 ? 0 : (i + (dir < 0 ? -1 : 1) + n) % n;
            SetChoiceAt(d, h, e, i);
            Changed();
        } else if (d.kind == cat::Kind::Slider) {
            const float step = d.step * (coarse ? 5.0f : 1.0f) * float(dir == 0 ? 1 : dir);
            SetSlider(d, FloatValue(d, e) + step);
        }
    }
    void SetSlider(const cat::Desc& d, float v) {
        v = std::clamp(v, d.min, d.max);
        v = d.min + std::round((v - d.min) / d.step) * d.step;
        v = std::clamp(v, d.min, d.max);
        const cat::Ref r = d.ref(*h.edit);
        if (auto* i = std::get_if<int*>(&r)) {
            const int nv = int(std::lround(v));
            if (nv == **i) return;
            **i = nv;
        } else if (auto* f = std::get_if<float*>(&r)) {
            if (std::fabs(**f - v) < 1e-5f) return;
            **f = v;
        }
        Changed();
    }
    void ResetRow(const cat::Desc& d) {
        if (!cat::Resettable(d) || !AvailOf(d, h).enabled) return; // REVLAUNCH19: Resettable (kNoReset = save folder)
        Settings before = *h.edit;
        cat::ApplyDefault(d, *h.edit);
        if (!cat::SameValue(d, before, *h.edit)) {
            Changed();
            SetStatus(std::string(L("Standaard: ", "Default: ")) + cat::T(d.label, h.en), false);
        }
    }
    void Activate(const cat::Desc& d) {
        const cat::Avail av = AvailOf(d, h);
        if (!std::strcmp(d.id, "act_play")) {
            if (av.enabled) Leave(2);
            return;
        }
        if (!av.enabled) return;
        switch (d.kind) {
        case cat::Kind::Toggle:
        case cat::Kind::Choice: ChangeValue(d, 0, false); return;
        case cat::Kind::Slider: return;
        case cat::Kind::Path: {
            if (!h.pick_folder) return;
            const cat::Ref r = d.ref(*h.edit);
            std::string* p = *std::get_if<std::string*>(&r);
            std::string start = *p;
            if (start.empty() && !std::strcmp(d.id, "nand_root")) start = h.nand_root;
            const std::string got = h.pick_folder(cat::T(d.label, h.en), start);
            if (!got.empty() && got != *p) {
                *p = got;
                Changed();
            }
            return;
        }
        case cat::Kind::Bindings: st.sub_calibration=false;st.sub_controls = true; return;
        case cat::Kind::Info:
            if (!std::strcmp(d.id, "game_dir") && h.open_folder) h.open_folder(h.game_dir);
            if (!std::strcmp(d.id, "settings_file") && h.open_folder) h.open_folder(h.settings_path);
            return;
        case cat::Kind::Action: break;
        }
        if (!std::strcmp(d.id, "act_gyro_calibration")) {
            st.sub_calibration=true;st.sub_controls=true;
        } else if (!std::strcmp(d.id, "act_test_controls")) {
            st.sub_calibration=false;st.sub_controls = true;
        } else if (!std::strcmp(d.id, "act_open_saves")) {
            if (h.open_folder) h.open_folder(h.nand_root);
        } else if (!std::strcmp(d.id, "act_open_logs")) {
            if (h.open_folder) h.open_folder(h.log_dir);
        } else if (!std::strcmp(d.id, "act_reset_all")) {
            st.dialog = Dialog::ResetAll;
            st.dlg_sel = 1;
            st.dlg_binds = false;
        } else if (h.action) {
            std::string title;
            bool err = false;
            const std::string msg = h.action(d.id, title, err);
            if (!msg.empty()) ShowMessage(title, msg, err);
        }
    }
    void ShowMessage(const std::string& title, const std::string& body, bool err) {
        st.msg_title = title;
        st.msg_body = body;
        st.msg_error = err;
        st.dialog = Dialog::Message;
        st.dlg_sel = 0;
    }
    bool DoSave() {
        if (!h.save) return false;
        std::string status;
        const bool ok = h.save(status);
        SetStatus(status, !ok);
        return ok;
    }
    void Leave(int what) { // 1 back, 2 play
        if (DirtyCount(h) > 0) {
            st.pending = what;
            st.dialog = Dialog::Unsaved;
            st.dlg_sel = 0;
            return;
        }
        result = what == 2 ? Result::Play : Result::Back;
    }
    void ResetTab() {
        st.dialog = Dialog::ResetTab;
        st.dlg_sel = 1;
    }
    std::vector<const cat::Desc*> TabResetRows() const {
        std::vector<const cat::Desc*> v;
        for (const Row& r : RowsOf(h, st.tab)) {
            if (!cat::Resettable(*r.d) || EnvLocked(*r.d)) continue; // REVLAUNCH19: Resettable
            Settings t = *h.edit;
            cat::ApplyDefault(*r.d, t);
            if (!cat::SameValue(*r.d, t, *h.edit)) v.push_back(r.d);
        }
        return v;
    }

    // ---- drawing
    void Header() {
        ImDrawList* dl = x.dl;
        DrawBrand(dl, h, ImVec2(a.x + P(36), a.y + P(16)), P(56), (h.f.word ? h.f.word->FontSize : P(60)) * 0.72f, false);
        float cx = b.x - P(36);
        // chips right to left: controller + layout, game state, settings file problem
        std::string pad;
        switch (h.pad) {
        case Pad::PlayStation: pad = "PlayStation"; break;
        case Pad::Xbox: pad = "Xbox"; break;
        case Pad::Switch: pad = "Nintendo Switch"; break;
        default: pad = L("Toetsenbord + muis", "Keyboard + mouse");
        }
        const cat::Desc* pd = cat::Find(h.pad == Pad::Keyboard ? "preset_keyboard" : "preset_gamepad");
        if (pd) pad += "  \xC2\xB7  " + ValueText(*pd, h, *h.edit);
        const float w2 = ChipWidth(x, pad.c_str());
        Chip(x, ImVec2(cx - w2, a.y + P(30)), pad.c_str(), kMuted);
        cx -= w2 + P(8);
        const char* c1 = h.in_game ? L("Spel draait", "Game running")
                                   : !h.disc_done ? L("Schijf controleren...", "Checking disc...")
                                   : h.disc_ok ? L("Schijf gecontroleerd", "Disc checked") : L("Schijf klopt niet", "Disc problem");
        const ImU32 c1c = h.in_game || h.disc_ok ? kGreen : !h.disc_done ? kOrange : kRed;
        const float w1 = ChipWidth(x, c1);
        Chip(x, ImVec2(cx - w1, a.y + P(30)), c1, c1c);
        cx -= w1 + P(8);
        if (!h.settings_problem.empty()) {
            const char* c0 = L("Instellingenbestand heeft een fout", "Settings file has an error");
            const float w0 = ChipWidth(x, c0);
            const ImVec2 p0(cx - w0, a.y + P(30)), p1(cx, a.y + P(30) + h.f.small->FontSize + P(4));
            Chip(x, p0, c0, kOrange);
            Track(c0, p0, p1);
            if (Clicked(p0, p1) && !Modal()) { // jump to the settings file row
                st.tab = int(cat::Tab::Geavanceerd);
                st.focus[st.tab] = "settings_file";
                st.scroll_to_focus = true;
                st.sub_controls = false;
            }
        }
    }
    bool Modal() const { return st.dialog != Dialog::None; }
    // a NWII_* variable the player set wins over the row: reset never touches it
    bool EnvLocked(const cat::Desc& d) const { return d.env && h.ctx.env && h.ctx.env(d.env) && *h.ctx.env(d.env); }

    void Tabs() {
        ImDrawList* dl = x.dl;
        const float tab_y = a.y + P(92), tab_h = P(38);
        const int nt = int(cat::Tab::Count);
        float total = 0, tw[8];
        ImFont* f = h.f.tab ? h.f.tab : h.f.bold;
        for (int i = 0; i < nt; ++i) {
            tw[i] = TextSize(f, cat::T(cat::kTabNames[i], h.en)).x + P(34);
            total += tw[i];
        }
        float tx = a.x + (W - total) * 0.5f;
        dl->AddLine(ImVec2(a.x + P(36), tab_y + tab_h), ImVec2(b.x - P(36), tab_y + tab_h), A(x.acc, 0.30f), P(1));
        // L1 / R1 (Q / E) beside the strip
        const float gh = P(26), gy = tab_y + (tab_h - gh) * 0.5f;
        if (h.pad == Pad::Keyboard) {
            KeyCap(dl, h.f.small, ImVec2(tx - P(14) - gh, gy), gh, "Q");
            KeyCap(dl, h.f.small, ImVec2(tx + total + P(14), gy), gh, "E");
        } else {
            const char* l1 = h.pad == Pad::PlayStation ? "L1" : h.pad == Pad::Switch ? "L" : "LB";
            const char* r1 = h.pad == Pad::PlayStation ? "R1" : h.pad == Pad::Switch ? "R" : "RB";
            const float lw = TextSize(h.f.small, l1).x + gh * 0.6f;
            KeyCap(dl, h.f.small, ImVec2(tx - P(14) - std::max(gh, lw), gy), gh, l1);
            KeyCap(dl, h.f.small, ImVec2(tx + total + P(14), gy), gh, r1);
        }
        for (int i = 0; i < nt; ++i) {
            const bool on = st.tab == i;
            const char* t = cat::T(cat::kTabNames[i], h.en);
            const ImVec2 ts = TextSize(f, t);
            const ImVec2 p0(tx, tab_y), p1(tx + tw[i], tab_y + tab_h);
            const bool hot = !Modal() && Hover(p0, p1);
            if (on) {
                dl->AddRectFilledMultiColor(p0, p1, A(x.acc, 0.0f), A(x.acc, 0.0f), A(x.acc, 0.22f), A(x.acc, 0.22f));
                dl->AddRectFilled(ImVec2(tx + P(8), p1.y - P(3)), ImVec2(p1.x - P(8), p1.y + P(1)), x.acc_hi, P(2));
                const float d = P(4.5f), mx = tx + tw[i] * 0.5f, my = p1.y + P(8);
                dl->AddQuadFilled(ImVec2(mx, my - d), ImVec2(mx + d, my), ImVec2(mx, my + d), ImVec2(mx - d, my), x.acc_hi);
            } else if (hot) {
                dl->AddRectFilled(p0, p1, A(x.acc, 0.08f), P(4));
            }
            Text(dl, f, ImVec2(tx + (tw[i] - ts.x) * 0.5f, tab_y + (tab_h - ts.y) * 0.5f),
                 on ? x.acc_hi : A(kParch, hot ? 0.95f : 0.72f), t);
            Track(t, p0, p1);
            if (!Modal() && Clicked(p0, p1) && st.tab != i) SwitchTab(i);
            tx += tw[i];
        }
    }
    void SwitchTab(int i) {
        const int nt = int(cat::Tab::Count);
        st.tab = (i % nt + nt) % nt;
        st.sub_controls = false;
        st.scroll_to_focus = true;
    }

    // One row; returns its height. Mouse: label area focuses, the editor area edits, right click = default.
    float DrawRow(const cat::Desc& d, ImVec2 p, float w, bool focused, bool input) {
        ImDrawList* dl = x.dl;
        const float hgt = P(40);
        const cat::Avail av = AvailOf(d, h);
        const bool en = h.en;
        const float alpha = av.enabled ? 1.0f : 0.42f;
        const ImVec2 ra(p.x, p.y), rb(p.x + w, p.y + hgt);
        const char* label = cat::T(d.label, en);
        if (!std::strcmp(d.id, "act_play")) { // the play row: a big button (Dusklight's PRELAUNCH launch button)
            const ImVec2 bb(rb.x, p.y + P(54));
            const float ga = av.enabled ? 1.0f : 0.45f;
            dl->AddRectFilledMultiColor(ra, bb, A(IM_COL32(150, 112, 50, 235), ga), A(IM_COL32(96, 70, 30, 235), ga),
                                        A(IM_COL32(96, 70, 30, 235), ga), A(IM_COL32(150, 112, 50, 235), ga));
            dl->AddRect(ra, bb, A(x.acc_hi, 0.85f * ga), P(7), 0, P(1.2f));
            const float cy = (ra.y + bb.y) * 0.5f;
            dl->AddTriangleFilled(ImVec2(ra.x + P(26), cy - P(11)), ImVec2(ra.x + P(26), cy + P(11)), ImVec2(ra.x + P(44), cy),
                                  A(x.acc_hi, ga));
            const char* t = en ? "PLAY" : "SPELEN";
            dl->AddText(h.f.menu, h.f.menu->FontSize * 0.86f, ImVec2(ra.x + P(60), cy - h.f.menu->FontSize * 0.45f),
                        A(IM_COL32(255, 244, 214, 255), ga), t);
            const char* sub = av.enabled ? "The Legend of Zelda: Skyward Sword  \xC2\xB7  USA 1.00"
                                         : cat::T(av.why, en);
            TextRight(dl, h.f.small, bb.x - P(18), cy - h.f.small->FontSize * 0.5f, A(kParch, 0.85f), sub);
            if (focused) {
                dl->AddRect(ImVec2(ra.x - P(3), ra.y - P(3)), ImVec2(bb.x + P(3), bb.y + P(3)), A(x.acc_hi, 0.35f), P(9), 0, P(4));
                dl->AddRect(ImVec2(ra.x - P(1), ra.y - P(1)), ImVec2(bb.x + P(1), bb.y + P(1)), x.acc_hi, P(8), 0, P(2));
            }
            Track(label, ra, bb);
            if (input) {
                if (Hover(ra, bb) && MouseMoved()) st.focus[st.tab] = d.id;
                if (Clicked(ra, bb)) Activate(d);
            }
            return P(54) + P(10);
        }
        if (focused) {
            dl->AddRectFilled(ra, rb, A(x.acc, 0.12f), P(6));
            dl->AddRect(ImVec2(ra.x - P(2), ra.y - P(2)), ImVec2(rb.x + P(2), rb.y + P(2)), A(x.acc, 0.28f), P(8), 0, P(4));
            dl->AddRect(ra, rb, x.acc_hi, P(6), 0, P(2));
        }
        const float ty = p.y + (hgt - h.f.body->FontSize) * 0.5f;
        float lx = p.x + P(16);
        if (Differs(d, *h.edit, *h.saved) || (d.kind == cat::Kind::Bindings && BindsDiffer(*h.edit, *h.saved)))
            dl->AddCircleFilled(ImVec2(p.x + P(8), p.y + hgt * 0.5f), P(3), x.acc_hi); // changed, not saved yet
        const ImU32 lc = A((d.flags & cat::kDanger) ? kOrange : kParch, alpha);
        ImFont* lf = focused ? h.f.bold : h.f.body;
        Text(dl, lf, ImVec2(lx, ty), lc, label);
        lx += TextSize(lf, label).x + P(10);
        const float chip_y = p.y + (hgt - h.f.small->FontSize - P(4)) * 0.5f;
        if (NeedsRestartChip(d, h)) lx += Chip(x, ImVec2(lx, chip_y), L("na herstart", "restart"), kOrange, true) + P(6);
        if (d.flags & cat::kExperimental) lx += Chip(x, ImVec2(lx, chip_y), L("experimenteel", "experimental"), kBlue) + P(6);
        const float rx = rb.x - P(16), cy = p.y + hgt * 0.5f;
        const std::string v = ValueText(d, h, *h.edit);
        ImVec2 ea(rb.x - P(250), ra.y), eb = rb; // the editor area (mouse)
        switch (d.kind) {
        case cat::Kind::Toggle: {
            const cat::Ref r = d.ref(*h.edit);
            const bool on = **std::get_if<bool*>(&r);
            const float pw = P(46), ph = P(24);
            const ImVec2 pa(rx - pw, cy - ph * 0.5f), pb(rx, cy + ph * 0.5f);
            dl->AddRectFilled(pa, pb, on ? A(x.acc, 0.95f * alpha) : A(IM_COL32(70, 56, 36, 255), alpha), ph * 0.5f);
            dl->AddCircleFilled(ImVec2(on ? pb.x - ph * 0.5f : pa.x + ph * 0.5f, cy), ph * 0.5f - P(3),
                                on ? A(IM_COL32(40, 28, 12, 255), alpha) : A(kMuted, alpha));
            TextRight(dl, h.f.body, pa.x - P(10), ty, A(on ? kParch : kMuted, alpha), v.c_str());
            ea = ImVec2(pa.x - P(70), ra.y);
            break;
        }
        case cat::Kind::Choice: {
            const float vw = std::max(P(150), TextSize(h.f.bold, v.c_str()).x + P(10));
            const ImU32 cc = focused ? x.acc_hi : A(kMuted, 0.6f * alpha);
            Chevron(dl, ImVec2(rx - P(4), cy), P(6), true, cc);
            const float vx = rx - P(18) - vw;
            Chevron(dl, ImVec2(vx - P(8), cy), P(6), false, cc);
            const ImVec2 ts = TextSize(h.f.bold, v.c_str());
            Text(dl, h.f.bold, ImVec2(vx + (vw - ts.x) * 0.5f, ty), A(kParch, alpha), v.c_str());
            const ChoiceList cl = Choices(d, h);
            const int n = int(cl.labels.size()), sel = CurrentChoice(d, h, *h.edit);
            if (n <= 7 && n > 1) {
                const float gap = P(9), x0 = vx + vw * 0.5f - (n - 1) * gap * 0.5f;
                for (int i = 0; i < n; ++i)
                    dl->AddCircleFilled(ImVec2(x0 + i * gap, rb.y - P(5)), P(i == sel ? 2.4f : 1.6f),
                                        i == sel ? A(x.acc, alpha) : A(kDim, 0.6f * alpha));
            }
            const ImVec2 pa(vx - P(20), ra.y), pb(vx + P(8), rb.y), na(rx - P(14), ra.y), nb(rb.x, rb.y);
            Track(std::string(label) + "::prev", pa, pb);
            Track(std::string(label) + "::next", na, nb);
            ea = ImVec2(vx - P(20), ra.y);
            if (input && av.enabled && ImGui::IsMouseClicked(0)) {
                if (Hover(pa, pb)) { st.focus[st.tab] = d.id; ChangeValue(d, -1, false); }
                else if (Hover(ImVec2(pb.x, ra.y), rb)) { st.focus[st.tab] = d.id; ChangeValue(d, +1, false); }
            }
            break;
        }
        case cat::Kind::Slider: {
            const float tw = P(150), vx = rx - P(64);
            TextRight(dl, h.f.bold, rx, ty, A(kParch, alpha), v.c_str());
            const ImVec2 ta(vx - tw, cy - P(3)), tb(vx - P(8), cy + P(3));
            dl->AddRectFilled(ta, tb, A(IM_COL32(70, 56, 36, 255), alpha), P(3));
            const float fr = std::clamp((FloatValue(d, *h.edit) - d.min) / std::max(1e-6f, d.max - d.min), 0.0f, 1.0f);
            dl->AddRectFilled(ta, ImVec2(ta.x + (tb.x - ta.x) * fr, tb.y), A(x.acc, alpha), P(3));
            dl->AddCircleFilled(ImVec2(ta.x + (tb.x - ta.x) * fr, cy), P(focused ? 8.5f : 7), A(focused ? x.acc_hi : x.acc, alpha));
            const ImVec2 ha(ta.x - P(8), ra.y), hb(tb.x + P(8), rb.y);
            Track(std::string(label) + "::track", ha, hb);
            ea = ImVec2(ta.x - P(8), ra.y);
            if (input && av.enabled) {
                if (Clicked(ha, hb)) {
                    st.dragging = true;
                    st.drag_id = d.id;
                    st.focus[st.tab] = d.id;
                }
                if (st.dragging && st.drag_id == d.id) {
                    if (ImGui::IsMouseDown(0)) {
                        const float mx = ImGui::GetMousePos().x;
                        SetSlider(d, d.min + (d.max - d.min) * std::clamp((mx - ta.x) / std::max(1.0f, tb.x - ta.x), 0.0f, 1.0f));
                    } else {
                        st.dragging = false;
                    }
                }
            }
            break;
        }
        case cat::Kind::Path: {
            std::string t = v.empty() ? (en ? "(default)" : "(standaard)") : v;
            if (v.empty() && !std::strcmp(d.id, "nand_root") && !h.nand_root.empty()) t = h.nand_root;
            t = Shorten(t, 34);
            FolderIcon(x, ImVec2(rx - P(20), cy - P(7)), P(20), A(x.acc, 0.9f * alpha));
            TextRight(dl, h.f.small, rx - P(30), cy - h.f.small->FontSize * 0.5f, A(kMuted, alpha), t.c_str());
            ea = ra; // the whole row opens the folder picker
            break;
        }
        case cat::Kind::Action:
        case cat::Kind::Bindings:
            Chevron(dl, ImVec2(rx - P(4), cy), P(6), true, focused ? x.acc_hi : A(kMuted, 0.7f * alpha));
            ea = ra;
            break;
        case cat::Kind::Info: {
            if (!std::strcmp(d.id, "game_dir")) {
                const float tx = rx - P(30) - TextSize(h.f.bold, v.c_str()).x;
                if (h.disc_done) {
                    if (h.disc_ok) Check(x, ImVec2(tx - P(14), cy), P(6), kGreen);
                    else Cross(x, ImVec2(tx - P(14), cy), P(5), kRed);
                }
                Text(dl, h.f.bold, ImVec2(tx, ty), kParch, v.c_str());
                FolderIcon(x, ImVec2(rx - P(20), cy - P(7)), P(20), A(x.acc, 0.9f));
            } else {
                TextRight(dl, h.f.small, rx, cy - h.f.small->FontSize * 0.5f, A(kMuted, alpha), v.c_str());
            }
            ea = ra;
            break;
        }
        }
        Track(label, ra, rb);
        Track(std::string(label) + "::value", ea, eb);
        if (input) {
            if (Hover(ra, rb) && MouseMoved() && !st.dragging) st.focus[st.tab] = d.id;
            if (Clicked(ra, rb, 1)) {
                st.focus[st.tab] = d.id;
                ResetRow(d);
            }
            if (Clicked(ea, eb) && d.kind != cat::Kind::Choice && d.kind != cat::Kind::Slider) {
                st.focus[st.tab] = d.id;
                Activate(d);
            } else if (Clicked(ra, rb)) {
                st.focus[st.tab] = d.id;
            }
        }
        return hgt + P(4);
    }

    void Description(const cat::Desc& d, ImVec2 pa, ImVec2 pb) {
        ImDrawList* dl = x.dl;
        const bool en = h.en;
        const float w = pb.x - pa.x;
        float y = pa.y;
        Text(dl, h.f.head, ImVec2(pa.x, y), x.acc, cat::T(d.label, en));
        y += h.f.head->FontSize + P(10);
        float cx = pa.x;
        if (const char* ap = ApplyLabel(d.apply, h.in_game, en)) {
            const bool restart = d.apply == cat::Apply::Restart || (d.apply == cat::Apply::AtBoot && h.in_game);
            const ImU32 c = d.apply == cat::Apply::Live ? kGreen : restart ? kOrange : kBlue;
            cx += Chip(x, ImVec2(cx, y), ap, c, restart) + P(6);
        }
        if (d.flags & cat::kExperimental) cx += Chip(x, ImVec2(cx, y), L("Experimenteel", "Experimental"), kBlue) + P(6);
        if (d.status == cat::Status::New || d.status == cat::Status::EnvOnly)
            cx += Chip(x, ImVec2(cx, y), L("Nieuw", "New"), x.acc) + P(6);
        y += h.f.small->FontSize + P(18);
        dl->AddLine(ImVec2(pa.x, y - P(8)), ImVec2(pb.x, y - P(8)), A(x.acc, 0.25f), P(1));
        y += Wrapped(dl, h.f.body, ImVec2(pa.x, y), w, kParch, cat::T(d.help, en)) + P(14);
        const cat::Avail av = AvailOf(d, h);
        if (av.why.nl) {
            const char* why = cat::T(av.why, en);
            const float hh = TextSize(h.f.small, why, 0, w - P(24)).y + P(14);
            dl->AddRectFilled(ImVec2(pa.x, y), ImVec2(pb.x, y + hh), A(kOrange, 0.12f), P(5));
            dl->AddRectFilled(ImVec2(pa.x, y), ImVec2(pa.x + P(3), y + hh), kOrange);
            Wrapped(dl, h.f.small, ImVec2(pa.x + P(14), y + P(7)), w - P(24), kOrange, why);
            y += hh + P(12);
        }
        if (d.env && h.ctx.env && h.ctx.env(d.env) && *h.ctx.env(d.env)) {
            char b2[160];
            std::snprintf(b2, sizeof b2, "%s=%s", d.env, h.ctx.env(d.env));
            y += Wrapped(dl, h.f.small, ImVec2(pa.x, y), w, kDim, b2) + P(8);
        }
        // the disc row: the game's own logo (from the player's disc) + the checks
        if (!std::strcmp(d.id, "game_dir")) {
            float lh = 0;
            const float lw = std::min(w * 0.42f, P(200));
            if (h.disc_logo && h.disc_logo_sz.x > 0) {
                lh = lw * h.disc_logo_sz.y / h.disc_logo_sz.x;
                dl->AddImage(h.disc_logo, ImVec2(pa.x, y), ImVec2(pa.x + lw, y + lh));
            }
            const float lx = h.disc_logo ? pa.x + lw + P(24) : pa.x + P(10);
            float cy = y + std::max(0.0f, (lh - float(h.disc_checks.size()) * (h.f.small->FontSize + P(8))) * 0.5f);
            for (size_t i = 0; i < h.disc_checks.size(); ++i) {
                const bool ok = i < h.disc_checks_ok.size() && h.disc_checks_ok[i];
                if (ok) Check(x, ImVec2(lx, cy + P(9)), P(5), kGreen);
                else Cross(x, ImVec2(lx, cy + P(9)), P(4), kRed);
                Text(dl, h.f.small, ImVec2(lx + P(12), cy), kMuted, h.disc_checks[i].c_str());
                cy += h.f.small->FontSize + P(8);
            }
            y = std::max(y + lh, cy) + P(10);
            y += Wrapped(dl, h.f.small, ImVec2(pa.x, y), w, kDim, h.game_dir.c_str()) + P(10);
        }
        if ((!std::strcmp(d.id, "save_backups") || !std::strcmp(d.id, "nand_root") || !std::strcmp(d.id, "act_open_saves")) &&
            !h.save_info.empty())
            y += Wrapped(dl, h.f.small, ImVec2(pa.x, y), w, kMuted, h.save_info.c_str()) + P(10);
        if (!std::strcmp(d.id, "nand_root") && !h.nand_root.empty())
            y += Wrapped(dl, h.f.small, ImVec2(pa.x, y), w, kDim, h.nand_root.c_str()) + P(10);
        if (!std::strcmp(d.id, "settings_file"))
            y += Wrapped(dl, h.f.small, ImVec2(pa.x, y), w, kDim, h.settings_path.c_str()) + P(10);
        // the choices
        if (d.kind == cat::Kind::Choice) {
            const ChoiceList cl = Choices(d, h);
            if (cl.labels.size() <= 8) {
                Text(dl, h.f.small, ImVec2(pa.x, y), kDim, L("Keuzes", "Choices"));
                y += h.f.small->FontSize + P(6);
                float cx2 = pa.x;
                const int cur = CurrentChoice(d, h, *h.edit);
                for (size_t i = 0; i < cl.labels.size(); ++i) {
                    const float cw = ChipWidth(x, cl.labels[i].c_str());
                    if (cx2 + cw > pb.x) {
                        cx2 = pa.x;
                        y += h.f.small->FontSize + P(10);
                    }
                    Chip(x, ImVec2(cx2, y), cl.labels[i].c_str(), int(i) == cur ? x.acc_hi : kDim);
                    cx2 += cw + P(6);
                }
                y += h.f.small->FontSize + P(18);
            }
        }
        // default + where it is stored, at the bottom of the pane
        if (d.section && cat::IsValueRow(d)) {
            const float my = pb.y - h.f.small->FontSize * 2 - P(8);
            std::string def = std::string(L("Standaard: ", "Default: ")) + DefaultText(d, h);
            if (d.flags & cat::kNoReset) // REVLAUNCH19: the save folder is never reset
                def = L("Standaard: blijft zoals SkyBreak Setup hem koos (wordt nooit teruggezet)",
                        "Default: stays as SkyBreak Setup chose it (never reset)");
            Text(dl, h.f.small, ImVec2(pa.x, my), kMuted, def.c_str());
            char buf[200];
            std::snprintf(buf, sizeof buf, "instellingen.toml  [%s]  %s%s%s", d.section, d.key, d.env ? "     " : "",
                          d.env ? d.env : "");
            Text(dl, h.f.small, ImVec2(pa.x, my + h.f.small->FontSize + P(4)), kDim, buf);
        }
    }

    void List(ImVec2 la, ImVec2 lb, const Keys& k, bool input) {
        ImDrawList* dl = x.dl;
        std::vector<Row> rows = RowsOf(h, st.tab);
        if (rows.empty()) return;
        std::string& focus = st.focus[st.tab];
        int fi = -1;
        for (size_t i = 0; i < rows.size(); ++i)
            if (focus == rows[i].d->id) fi = int(i);
        if (fi < 0) {
            fi = 0;
            focus = rows[0].d->id;
        }
        // keyboard / controller
        if (input) {
            const int n = int(rows.size());
            int nf = fi;
            const int page = std::max(1, int((lb.y - la.y) / P(44)) - 1);
            if (k.up) nf = (fi - 1 + n) % n;
            if (k.down) nf = (fi + 1) % n;
            if (k.page_up) nf = std::max(0, fi - page);
            if (k.page_down) nf = std::min(n - 1, fi + page);
            if (k.home) nf = 0;
            if (k.end) nf = n - 1;
            if (nf != fi) {
                fi = nf;
                focus = rows[size_t(fi)].d->id;
                st.scroll_to_focus = true;
            }
            const cat::Desc& fd = *rows[size_t(fi)].d;
            if (k.left) ChangeValue(fd, -1, k.shift);
            if (k.right) ChangeValue(fd, +1, k.shift);
            if (k.confirm) Activate(fd);
            if (k.def) ResetRow(fd);
        }
        // layout pass: content height and the focused row's position
        float content = 0, fy = 0, fh = 0;
        const char* group = nullptr;
        for (size_t i = 0; i < rows.size(); ++i) {
            const char* g = cat::T(rows[i].group, h.en);
            if (!group || std::strcmp(group, g)) {
                content += (group ? P(6) : 0) + h.f.caps->FontSize + P(10);
                group = g;
            }
            if (int(i) == fi) { fy = content; fh = RowHeight(x, *rows[i].d); }
            content += RowHeight(x, *rows[i].d);
        }
        const float vh = lb.y - la.y;
        float& scroll = st.scroll[st.tab];
        if (input && Hover(la, lb) && ImGui::GetIO().MouseWheel != 0.0f) {
            scroll -= ImGui::GetIO().MouseWheel * P(66);
            st.scroll_to_focus = false;
        }
        if (st.scroll_to_focus) {
            if (fy < scroll || fy + fh > scroll + vh) scroll = fy - vh * 0.4f;
            st.scroll_to_focus = false;
        }
        scroll = content > vh ? std::clamp(scroll, 0.0f, content - vh) : 0.0f;
        // draw
        dl->PushClipRect(ImVec2(la.x - P(6), la.y - P(4)), ImVec2(lb.x + P(6), lb.y + P(4)), true);
        float y = la.y - scroll;
        group = nullptr;
        for (size_t i = 0; i < rows.size(); ++i) {
            const char* g = cat::T(rows[i].group, h.en);
            if (!group || std::strcmp(group, g)) {
                if (group) y += P(6);
                std::string up = g;
                for (char& c : up) c = char(std::toupper(static_cast<unsigned char>(c)));
                Text(dl, h.f.caps, ImVec2(la.x + P(4), y + P(4)), A(x.acc, 0.85f), up.c_str());
                const float gx = la.x + P(12) + TextSize(h.f.caps, up.c_str()).x;
                dl->AddLine(ImVec2(gx, y + P(4) + h.f.caps->FontSize * 0.55f), ImVec2(lb.x, y + P(4) + h.f.caps->FontSize * 0.55f),
                            A(x.acc, 0.18f), P(1));
                y += h.f.caps->FontSize + P(10);
                group = g;
            }
            const float rh = RowHeight(x, *rows[i].d);
            const bool visible = y + rh > la.y - P(4) && y < lb.y + P(4);
            // rows outside the view are not drawn and take no mouse input (their autotest target is not tracked)
            if (visible) DrawRow(*rows[i].d, ImVec2(la.x, y), lb.x - la.x - P(10), int(i) == fi,
                                 input && y >= la.y - P(2) && y + P(40) <= lb.y + P(2));
            y += rh;
        }
        dl->PopClipRect();
        if (content > vh) { // scrollbar + "more below"
            const float tx = lb.x + P(2);
            dl->AddRectFilled(ImVec2(tx, la.y), ImVec2(tx + P(4), lb.y), IM_COL32(0, 0, 0, 90), P(2));
            const float th = vh * vh / content, tyy = la.y + (vh - th) * (scroll / (content - vh));
            dl->AddRectFilled(ImVec2(tx, tyy), ImVec2(tx + P(4), tyy + th), A(x.acc, 0.7f), P(2));
            if (scroll + vh < content - 1) {
                dl->AddRectFilledMultiColor(ImVec2(la.x - P(6), lb.y - P(40)), ImVec2(lb.x + P(1), lb.y + P(4)),
                                            IM_COL32(16, 12, 8, 0), IM_COL32(16, 12, 8, 0), IM_COL32(16, 12, 8, 235),
                                            IM_COL32(16, 12, 8, 235));
                const char* more = L("meer hieronder", "more below");
                const ImVec2 ms = TextSize(h.f.small, more);
                const ImVec2 mp(lb.x - ms.x - P(26), lb.y - ms.y - P(2));
                dl->AddRectFilled(ImVec2(mp.x - P(22), mp.y - P(3)), ImVec2(mp.x + ms.x + P(8), mp.y + ms.y + P(3)),
                                  IM_COL32(30, 24, 16, 240), P(4));
                dl->AddTriangleFilled(ImVec2(mp.x - P(16), mp.y + P(5)), ImVec2(mp.x - P(6), mp.y + P(5)),
                                      ImVec2(mp.x - P(11), mp.y + P(12)), x.acc);
                Text(dl, h.f.small, mp, x.acc, more);
            }
        }
    }

    // Footer: hints (clickable) left, dirty/restart counters right.
    void Footer(bool input) {
        ImDrawList* dl = x.dl;
        const float fh = P(28), fy = b.y - P(50);
        float fx = a.x + P(40);
        struct Hint { int pos; const char* key; const char* label; int what; };
        // what: 1 change, 2 back, 3 default, 4 save, 5 next tab, 6 play
        const Hint hints[] = {
            {ConfirmPos(h), "Enter", L("Wijzigen", "Change"), 1},
            {BackPos(h), "Esc", L("Terug", "Back"), 2},
            {2, "Del", L("Standaard", "Default"), 3},
            {3, "Ctrl+S", L("Opslaan", "Save"), 4},
            {-1, h.pad == Pad::Keyboard ? "Q / E" : h.pad == Pad::PlayStation ? "L1 / R1" : h.pad == Pad::Switch ? "L / R" : "LB / RB",
             L("Tabblad", "Tab"), 5},
            {-1, h.pad == Pad::Keyboard ? "F5" : h.pad == Pad::PlayStation ? "Options" : h.pad == Pad::Switch ? "+" : "Menu",
             h.in_game ? L("Verder spelen", "Resume") : L("Spelen", "Play"), 6},
        };
        for (const Hint& hi : hints) {
            if (hi.what == 6 && !h.in_game && !h.disc_ok) continue;
            const float w = DrawHint(dl, h, ImVec2(fx, fy), fh, hi.pos, hi.key, hi.label);
            const ImVec2 p0(fx - P(4), fy - P(3)), p1(fx + w + P(4), fy + fh + P(3));
            Track(hi.label, p0, p1);
            if (input && Hover(p0, p1)) dl->AddRect(p0, p1, A(x.acc, 0.5f), P(5), 0, P(1));
            if (input && Clicked(p0, p1)) FooterAction(hi.what);
            fx += w + P(22);
        }
        // right: status (a few seconds after saving) or the dirty / restart counters
        float rx = b.x - P(40);
        const int restart = RestartCount(h), dirty = DirtyCount(h);
        if (restart > 0) {
            const char* now = L("Nu herstarten", "Restart now");
            const float bw = ChipWidth(x, now, true);
            const ImVec2 p0(rx - bw, fy + P(3)), p1(rx, fy + P(3) + h.f.small->FontSize + P(4));
            Chip(x, p0, now, kOrange, true);
            Track(now, p0, p1);
            if (input && Clicked(p0, p1)) {
                st.dialog = Dialog::Restart;
                st.dlg_sel = 1;
            }
            rx -= bw + P(10);
            char b2[64];
            std::snprintf(b2, sizeof b2, h.en ? "%d after restart" : "%d na herstart", restart);
            TextRight(dl, h.f.small, rx, fy + (fh - h.f.small->FontSize) * 0.5f, kOrange, b2);
            rx -= TextSize(h.f.small, b2).x + P(14);
        }
        if (h.now - st.status_t < 5.0 && !st.status.empty()) {
            const float maxw = std::max(P(120), rx - (fx + P(10)));
            std::string s = st.status;
            while (s.size() > 8 && TextSize(h.f.small, s.c_str()).x > maxw) s = s.substr(0, s.size() - 8) + "\xE2\x80\xA6";
            TextRight(dl, h.f.small, rx, fy + (fh - h.f.small->FontSize) * 0.5f, st.status_error ? kRed : kGreen, s.c_str());
        } else if (dirty) {
            char buf[128];
            std::snprintf(buf, sizeof buf, h.en ? "%d unsaved change%s" : "%d wijziging%s niet opgeslagen", dirty,
                          dirty == 1 ? "" : (h.en ? "s" : "en"));
            dl->AddCircleFilled(ImVec2(rx - TextSize(h.f.small, buf).x - P(10), fy + fh * 0.5f), P(3), x.acc_hi);
            TextRight(dl, h.f.small, rx, fy + (fh - h.f.small->FontSize) * 0.5f, x.acc_hi, buf);
        }
    }
    void FooterAction(int what) {
        std::vector<Row> rows = RowsOf(h, st.tab);
        const cat::Desc* fd = nullptr;
        for (const Row& r : rows)
            if (st.focus[st.tab] == r.d->id) fd = r.d;
        switch (what) {
        case 1: if (fd) Activate(*fd); break;
        case 2: if (st.sub_controls) st.sub_controls = false; else Leave(1); break;
        case 3: if (fd) ResetRow(*fd); break;
        case 4: DoSave(); break;
        case 5: SwitchTab(st.tab + 1); break;
        case 6: Leave(2); break;
        }
    }

    // ---- dialogs
    struct Btn { const char* label; int id; };
    // Draws a centred dialog with `lines` and buttons; returns the id of the button chosen this frame (-1 none).
    int DialogBox(const char* title, const std::vector<std::string>& lines, const std::vector<bool>& restart_marks,
                  const char* note, const std::vector<Btn>& btns, int back_id, int west_id, int north_id,
                  bool* checkbox = nullptr, const char* checkbox_label = nullptr) {
        ImDrawList* dl = x.dl;
        dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, 150));
        const float w = std::min(W - P(60), P(640));
        const float lh = h.f.body->FontSize + P(10);
        const size_t shown = std::min<size_t>(lines.size(), 10);
        const float note_h = note ? TextSize(h.f.small, note, 0, w - P(48)).y + P(14) : 0;
        const float cb_h = checkbox ? h.f.body->FontSize + P(16) : 0;
        const float hh = P(20) + h.f.head->FontSize + P(14) + shown * lh + (lines.size() > shown ? lh : 0) + note_h + cb_h +
                         P(20) + P(66);
        const ImVec2 da(a.x + (W - w) * 0.5f, a.y + (H - hh) * 0.5f), db(da.x + w, da.y + hh);
        dl->AddRectFilled(da, db, IM_COL32(22, 17, 11, 250), P(10));
        dl->AddRect(da, db, A(x.acc, 0.7f), P(10), 0, P(1.2f));
        float y = da.y + P(20);
        Text(dl, h.f.head, ImVec2(da.x + P(24), y), x.acc, title);
        y += h.f.head->FontSize + P(14);
        for (size_t i = 0; i < shown; ++i) {
            Text(dl, h.f.body, ImVec2(da.x + P(36), y), kParch, lines[i].c_str());
            dl->AddCircleFilled(ImVec2(da.x + P(26), y + h.f.body->FontSize * 0.55f), P(3), x.acc_hi);
            if (i < restart_marks.size() && restart_marks[i])
                Chip(x, ImVec2(da.x + P(44) + TextSize(h.f.body, lines[i].c_str()).x, y + P(2)), L("na herstart", "restart"),
                     kOrange, true);
            y += lh;
        }
        if (lines.size() > shown) {
            char more[64];
            std::snprintf(more, sizeof more, h.en ? "... and %zu more" : "... en nog %zu", lines.size() - shown);
            Text(dl, h.f.body, ImVec2(da.x + P(36), y), kMuted, more);
            y += lh;
        }
        if (note) {
            y += P(4);
            y += Wrapped(dl, h.f.small, ImVec2(da.x + P(24), y), w - P(48), kMuted, note) + P(10);
        }
        int chosen = -1;
        if (checkbox) {
            const ImVec2 c0(da.x + P(24), y), c1(c0.x + P(20), y + P(20));
            dl->AddRect(c0, c1, x.acc, P(3), 0, P(1.5f));
            if (*checkbox) Check(x, ImVec2(c0.x + P(10), c0.y + P(10)), P(6), x.acc_hi);
            Text(dl, h.f.body, ImVec2(c1.x + P(10), y), kParch, checkbox_label);
            const ImVec2 hb(c1.x + P(10) + TextSize(h.f.body, checkbox_label).x, c1.y);
            Track(checkbox_label, c0, hb);
            if (!dialog_fresh && Clicked(c0, hb)) *checkbox = !*checkbox;
            // REVLAUNCH19: also without a mouse - west (Square / X / Y) or Delete toggles it (dialogs with a checkbox
            // have no west button)
            if (west_id < 0) DrawHint(dl, h, ImVec2(hb.x + P(14), y - P(1)), P(22), 2, "Del", L("aan/uit", "on/off"));
            y += cb_h;
        }
        const int n = int(btns.size());
        st.dlg_sel = std::clamp(st.dlg_sel, 0, n - 1);
        const float bw = (w - P(48) - P(10) * (n - 1)) / n, by = db.y - P(66);
        for (int i = 0; i < n; ++i) {
            const ImVec2 ba(da.x + P(24) + i * (bw + P(10)), by), bb(ba.x + bw, by + P(44));
            const bool sel = st.dlg_sel == i;
            dl->AddRectFilled(ba, bb, sel ? IM_COL32(120, 90, 40, 255) : IM_COL32(60, 46, 28, 255), P(6));
            if (sel) {
                dl->AddRect(ImVec2(ba.x - P(2), ba.y - P(2)), ImVec2(bb.x + P(2), bb.y + P(2)), A(x.acc, 0.3f), P(8), 0, P(4));
                dl->AddRect(ba, bb, x.acc_hi, P(6), 0, P(2));
            }
            // glyph: the button that does this directly
            const int id = btns[size_t(i)].id;
            const int pos = id == back_id ? BackPos(h) : id == west_id ? 2 : id == north_id ? 3 : sel ? ConfirmPos(h) : -1;
            const float gh = P(24);
            const ImVec2 ts = TextSize(h.f.bold, btns[size_t(i)].label);
            float tx = ba.x + (bw - ts.x) * 0.5f;
            if (pos >= 0 && h.pad != Pad::Keyboard) {
                tx = ba.x + (bw - ts.x - gh - P(6)) * 0.5f;
                DrawPadGlyph(dl, h.f.bold, ImVec2(tx + gh * 0.5f, ba.y + P(22)), gh * 0.5f, h.pad, pos);
                tx += gh + P(6);
            }
            Text(dl, h.f.bold, ImVec2(tx, ba.y + (P(44) - ts.y) * 0.5f), kParch, btns[size_t(i)].label);
            Track(btns[size_t(i)].label, ba, bb);
            if (Hover(ba, bb) && MouseMoved()) st.dlg_sel = i;
            if (!dialog_fresh && Clicked(ba, bb)) chosen = id;
        }
        const Keys k = dialog_fresh ? Keys{} : ReadKeys(h);
        if (k.left || k.up) st.dlg_sel = (st.dlg_sel - 1 + n) % n;
        if (k.right || k.down) st.dlg_sel = (st.dlg_sel + 1) % n;
        if (k.confirm) chosen = btns[size_t(st.dlg_sel)].id;
        if (k.back && back_id >= 0) chosen = back_id;
        if (k.def && west_id >= 0) chosen = west_id;
        if (k.def && west_id < 0 && checkbox) *checkbox = !*checkbox; // REVLAUNCH19: checkbox without a mouse
        if (k.save && north_id >= 0) chosen = north_id;
        return chosen;
    }

    void Dialogs() {
        switch (st.dialog) {
        case Dialog::None: return;
        case Dialog::Unsaved: {
            std::vector<std::string> lines;
            std::vector<bool> marks;
            for (const cat::Desc& d : cat::All()) {
                if (!Differs(d, *h.edit, *h.saved)) continue;
                lines.push_back(std::string(cat::T(d.label, h.en)) + ":  " + ValueText(d, h, *h.saved) + "  \xE2\x86\x92  " +
                                ValueText(d, h, *h.edit));
                marks.push_back(NeedsRestartChip(d, h));
            }
            if (BindsDiffer(*h.edit, *h.saved)) {
                lines.push_back(L("Knoppen aanpassen: gewijzigd", "Button layout: changed"));
                marks.push_back(false);
            }
            std::string note = L("Opslaan schrijft instellingen.toml (je eigen opmerkingen blijven staan). Wijzigingen met "
                                 "'na herstart' gelden de volgende keer dat je SkyBreak start.",
                                 "Save writes instellingen.toml (your comments are kept). Changes marked 'restart' apply "
                                 "the next time you start SkyBreak.");
            if (!h.settings_problem.empty())
                note += L(" Je bestand heeft nu een fout: Opslaan bewaart het eerst als instellingen.toml.bak.",
                          " Your file has an error now: Save first keeps it as instellingen.toml.bak.");
            const int c = DialogBox(L("Wijzigingen opslaan?", "Save changes?"), lines, marks, note.c_str(),
                                    {{L("Opslaan", "Save"), 0}, {L("Niet opslaan", "Discard"), 1}, {L("Annuleren", "Cancel"), 2}},
                                    2, 1, 0);
            if (c == 0) {
                if (DoSave()) Finish();
                else { st.dialog = Dialog::None; st.pending = 0; }
            } else if (c == 1) {
                if (h.revert) h.revert();
                Finish();
            } else if (c == 2) {
                st.dialog = Dialog::None;
                st.pending = 0;
            }
            return;
        }
        case Dialog::ResetTab:
        case Dialog::ResetAll: {
            const bool all = st.dialog == Dialog::ResetAll;
            std::vector<const cat::Desc*> rows;
            if (all) {
                for (const cat::Desc& d : cat::All()) {
                    if (!cat::Resettable(d) || (h.in_game && (d.flags & cat::kLauncherOnly)) || EnvLocked(d)) continue; // REVLAUNCH19
                    Settings t = *h.edit;
                    cat::ApplyDefault(d, t);
                    if (!cat::SameValue(d, t, *h.edit)) rows.push_back(&d);
                }
            } else {
                rows = TabResetRows();
            }
            std::vector<std::string> lines;
            for (const cat::Desc* d : rows) {
                Settings t = *h.edit;
                cat::ApplyDefault(*d, t);
                lines.push_back(std::string(cat::T(d->label, h.en)) + ":  " + ValueText(*d, h, *h.edit) + "  \xE2\x86\x92  " +
                                ValueText(*d, h, t));
            }
            if (lines.empty()) lines.push_back(L("Alles staat al op de standaardwaarde.", "Everything is at its default already."));
            const char* note = all ? L("Je huidige instellingen.toml gaat eerst naar een kopie (.bak). Je save blijft staan.",
                                       "Your current instellingen.toml is copied to a backup (.bak) first. Your save is kept.")
                                   : L("Daarna nog Opslaan om het te bewaren.", "Save afterwards to keep it.");
            char title[96];
            std::snprintf(title, sizeof title, "%s", all ? L("Alle instellingen terugzetten?", "Reset all settings?")
                                                         : L("Dit tabblad terugzetten?", "Reset this tab?"));
            const int c = DialogBox(title, lines, {}, note,
                                    {{L("Terugzetten", "Reset"), 0}, {L("Annuleren", "Cancel"), 1}}, 1, -1, -1,
                                    all ? &st.dlg_binds : nullptr,
                                    all ? L("Ook de knoppen-indelingen terugzetten", "Also reset the button layouts") : nullptr);
            if (c == 0) {
                for (const cat::Desc* d : rows) cat::ApplyDefault(*d, *h.edit);
                if (all && st.dlg_binds) {
                    h.edit->bind_pad_hd.clear();
                    h.edit->bind_pad_classic.clear();
                    h.edit->bind_kbm.clear();
                    h.edit->bind_kb_classic.clear();
                }
                st.dialog = Dialog::None;
                if (!rows.empty() || (all && st.dlg_binds)) Changed();
                if (all) {
                    std::string title2;
                    bool err = false;
                    const std::string bak = h.action ? h.action("reset_all_backup", title2, err) : std::string();
                    if (DoSave()) SetStatus(std::string(L("Alles teruggezet en opgeslagen. ", "All reset and saved. ")) + bak, false);
                }
            } else if (c == 1) {
                st.dialog = Dialog::None;
            }
            return;
        }
        case Dialog::Message: {
            std::vector<std::string> lines;
            size_t p = 0;
            while (p <= st.msg_body.size()) {
                const size_t nl = st.msg_body.find('\n', p);
                lines.push_back(st.msg_body.substr(p, nl == std::string::npos ? std::string::npos : nl - p));
                if (nl == std::string::npos) break;
                p = nl + 1;
            }
            const int c = DialogBox(st.msg_title.c_str(), lines, {}, nullptr, {{"OK", 0}}, 0, -1, -1);
            if (c == 0) st.dialog = Dialog::None;
            return;
        }
        case Dialog::Restart: {
            const char* note = h.in_game ? L("Voortgang die je niet in het spel hebt opgeslagen gaat verloren.",
                                             "Progress you did not save in the game is lost.")
                                         : L("SkyBreak sluit en start meteen opnieuw, met je opgeslagen instellingen.",
                                             "SkyBreak closes and starts again at once, with your saved settings.");
            const int c = DialogBox(L("SkyBreak opnieuw starten?", "Restart SkyBreak?"), {}, {}, note,
                                    {{L("Opnieuw starten", "Restart"), 0}, {L("Annuleren", "Cancel"), 1}}, 1, -1, -1);
            if (c == 0) {
                st.dialog = Dialog::None;
                if (DirtyCount(h) > 0) DoSave();
                std::string t;
                bool err = false;
                if (h.action) {
                    const std::string m = h.action("restart_now", t, err);
                    if (!m.empty()) ShowMessage(t, m, err);
                }
            } else if (c == 1) {
                st.dialog = Dialog::None;
            }
            return;
        }
        case Dialog::Danger: {
            const cat::Desc* d = cat::Find(st.danger_id);
            const char* note = d ? cat::T(d->help, h.en) : "";
            const int c = DialogBox(L("Weet je het zeker?", "Are you sure?"), {d ? cat::T(d->label, h.en) : ""}, {}, note,
                                    {{L("Ja, uitzetten", "Yes, turn off"), 0}, {L("Nee", "No"), 1}}, 1, -1, -1);
            if (c == 0) {
                if (d && d->ref) {
                    const cat::Ref r = d->ref(*h.edit);
                    if (auto* p = std::get_if<bool*>(&r)) **p = false;
                    Changed();
                }
                st.dialog = Dialog::None;
            } else if (c == 1) {
                st.dialog = Dialog::None;
            }
            return;
        }
        }
    }
    void Finish() {
        st.dialog = Dialog::None;
        const int what = st.pending;
        st.pending = 0;
        if (what == 1) result = Result::Back;
        if (what == 2) result = Result::Play;
    }

    // ---- CTRL18-UI's controls tab as a sub page (ImGui widgets, ImGui navigation)
    void SubControls(ImVec2 pa, ImVec2 pb) {
        ImDrawList* dl = x.dl;
        // breadcrumb + how to get back (the CTRL18-UI tab has its own title)
        const std::string crumb = std::string(L("BESTURING", "CONTROLS")) + "  \xC2\xB7  " +
                                  L("KNOPPEN EN TEST", "BUTTONS AND TEST");
        Text(dl, h.f.caps, ImVec2(pa.x + P(16), pa.y + P(14)), A(x.acc, 0.85f), crumb.c_str());
        const float bw = HintWidth(h, P(22), BackPos(h), "Esc", L("Terug naar de lijst", "Back to the list"));
        DrawHint(dl, h, ImVec2(pb.x - P(16) - bw, pa.y + P(8)), P(22), BackPos(h), "Esc", L("Terug naar de lijst", "Back to the list"));
        ImGui::SetCursorScreenPos(ImVec2(pa.x + P(16), pa.y + P(16) + h.f.head->FontSize + P(8)));
        ImGui::BeginChild("##ctrl18sub", ImVec2(pb.x - pa.x - P(32), pb.y - pa.y - h.f.head->FontSize - P(40)),
                          ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
        // REVLAUNCH19: keyboard / controller go straight INTO the controls page. Without this ImGui first selects the
        // child as ONE item of the page window and needs an extra, unmarked confirm to enter it (live run
        // 20261011-011618-rl19-sub: two D-pad presses left the nav on ##skybreak_page).
        if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && !ImGui::IsAnyItemActive() &&
            !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup) && !ControlsCapturing() &&
            (KP(ImGuiKey_UpArrow, false) || KP(ImGuiKey_DownArrow, false) || KP(ImGuiKey_LeftArrow, false) ||
             KP(ImGuiKey_RightArrow, false) || KP(ImGuiKey_GamepadDpadUp, false) || KP(ImGuiKey_GamepadDpadDown, false) ||
             KP(ImGuiKey_GamepadDpadLeft, false) || KP(ImGuiKey_GamepadDpadRight, false) ||
             KP(ImGuiKey_GamepadLStickUp, false) || KP(ImGuiKey_GamepadLStickDown, false)))
            ImGui::SetKeyboardFocusHere();
        ImGui::PushItemWidth(-P(260));
        const Settings before = *h.edit;
        if(st.sub_calibration)RenderGyroCalibration(*h.edit,h.controls);
        else RenderControlsTab(*h.edit, *h.saved, h.controls);
        ImGui::PopItemWidth();
        if (BindsDiffer(before, *h.edit) || DirtyCount(h) != 0) {
            // presets/options/rebinds changed inside the CTRL18-UI tab: preview like any other row
            bool any = BindsDiffer(before, *h.edit);
            for (const cat::Desc& d : cat::All()) any = any || Differs(d, before, *h.edit);
            if (any) Changed();
        }
        ImGui::EndChild();
    }
};

} // namespace

Result Draw(State& st, Host& h, ImVec2 a, ImVec2 b) {
    Page pg(st, h, a, b);
    const int nt = int(cat::Tab::Count);
    st.tab = std::clamp(st.tab, 0, nt - 1);
    ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                          ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse |
                          ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (!st.sub_controls || st.dialog != Dialog::None) fl |= ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoNavFocus;
    ImGui::SetNextWindowPos(a);
    ImGui::SetNextWindowSize(ImVec2(b.x - a.x, b.y - a.y));
    ImGui::Begin("##skybreak_page", nullptr, fl);
    pg.x.dl = ImGui::GetWindowDrawList();
    ImDrawList* dl = pg.x.dl;
    const bool modal = st.dialog != Dialog::None;
    Keys k = modal ? Keys{} : ReadKeys(h);
    const bool sub_busy = st.sub_controls && (ImGui::IsAnyItemActive() || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup) ||
                                              ControlsCapturing());
    if (!modal && !sub_busy) {
        if (k.tab_prev) pg.SwitchTab(st.tab - 1);
        if (k.tab_next) pg.SwitchTab(st.tab + 1);
        if (k.save) pg.DoSave();
        if (k.play) pg.Leave(2);
        if (k.reset_tab && !st.sub_controls) pg.ResetTab();
        if (k.back) {
            if (st.sub_controls) st.sub_controls = false;
            else pg.Leave(1);
            k = Keys{};
        }
    }
    pg.Header();
    pg.Tabs();
    // the content panel: list left, description right (or the controls sub page)
    const ImVec2 pa(a.x + pg.P(36), a.y + pg.P(146)), pb(b.x - pg.P(36), b.y - pg.P(66));
    dl->AddRectFilled(pa, pb, IM_COL32(16, 12, 8, 214), pg.P(10));
    dl->AddRect(pa, pb, A(h.accent, 0.35f), pg.P(10), 0, pg.P(1));
    const bool input = !modal && st.dialog == Dialog::None;
    if (st.sub_controls) {
        pg.SubControls(pa, pb);
    } else {
        const float split = pa.x + (pb.x - pa.x) * 0.565f;
        dl->AddLine(ImVec2(split, pa.y + pg.P(16)), ImVec2(split, pb.y - pg.P(16)), A(h.accent, 0.22f), pg.P(1));
        float wii_banner=0;
        if(st.tab==3) {
            runtime::input::ControllerStatus pads[4];runtime::input::controller_status(pads);
            if(pads[0].connected && std::strcmp(pads[0].type,"Wii Remote")==0) {
                const auto status=runtime::input::g_wii_ir_status[0].load();
                Text(dl,h.f.small,ImVec2(pa.x+pg.P(16),pa.y+pg.P(12)),
                     status==runtime::input::WiiIRStatus::Visible?kGreen:kOrange,
                     runtime::input::wii_ir_status_text(status));
                wii_banner=pg.P(28);
            }
        }
        const ImVec2 la(pa.x + pg.P(14), pa.y + pg.P(12)+wii_banner), lb(split - pg.P(14), pb.y - pg.P(12));
        if (input && sub_busy) k = Keys{};
        pg.List(la, lb, k, input);
        if (!st.sub_controls) { // (an action of this frame may have opened the sub page)
            const std::vector<Row> rows = RowsOf(h, st.tab);
            for (const Row& r : rows)
                if (st.focus[st.tab] == r.d->id) {
                    pg.Description(*r.d, ImVec2(split + pg.P(22), pa.y + pg.P(18)), ImVec2(pb.x - pg.P(22), pb.y - pg.P(16)));
                    break;
                }
        }
    }
    pg.Footer(input);
    pg.dialog_fresh = !modal; // opened by this frame's input (no dialog at the start of the frame)
    pg.Dialogs();
    ImGui::End();
    return pg.result;
}

std::string Query(const State& st, const Host& h, const std::string& q) {
    if (q == "tab") return std::to_string(st.tab);
    if (q == "tab_name") return cat::kTabNames[std::clamp(st.tab, 0, int(cat::Tab::Count) - 1)].nl;
    if (q == "focus") return st.focus[std::clamp(st.tab, 0, int(cat::Tab::Count) - 1)];
    if (q == "dirty") return std::to_string(DirtyCount(h));
    if (q == "restart_count") return std::to_string(RestartCount(h));
    if (q == "sub") return st.sub_controls ? (st.sub_calibration ? "calibration" : "controls") : "none";
    if (q == "dialog") {
        switch (st.dialog) {
        case Dialog::None: return "none";
        case Dialog::Unsaved: return "unsaved";
        case Dialog::ResetTab: return "reset_tab";
        case Dialog::ResetAll: return "reset_all";
        case Dialog::Message: return "message";
        case Dialog::Restart: return "restart";
        case Dialog::Danger: return "danger";
        }
    }
    if (q == "dialog_text") return st.msg_title + " | " + st.msg_body;
    if (q == "dialog_binds") return st.dlg_binds ? "1" : "0"; // REVLAUNCH19: reset-all "also the button layouts"
    if (q == "page_status") return st.status;
    if (q.rfind("avail.", 0) == 0) {
        const cat::Desc* d = cat::Find(q.substr(6));
        if (!d) return "?";
        if (h.in_game && (d->flags & cat::kLauncherOnly)) return "hidden";
        const cat::Avail av = AvailOf(*d, h);
        return !av.visible ? "hidden" : av.enabled ? "1" : "0";
    }
    if (q.rfind("row.", 0) == 0) { // the value text the row shows
        const cat::Desc* d = cat::Find(q.substr(4));
        return d ? ValueText(*d, h, *h.edit) : "?";
    }
    if (q.rfind("restart.", 0) == 0) {
        const cat::Desc* d = cat::Find(q.substr(8));
        return d ? (NeedsRestartChip(*d, h) ? "1" : "0") : "?";
    }
    return "?";
}

} // namespace nwii::ui::page
