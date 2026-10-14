// UI13: launcher (title screen), settings screens, in-game menu and controller toasts, drawn with
// Dear ImGui (MIT, v1.91.9b, SDL2 + OpenGL3 backends) in a Skyward Sword look built at run time
// from the player's own disc: a frame of the attract movie (THP) as background, the title logo
// from Title2D.arc and the game's font (normal_00.brfnt). Nothing from the game is shipped.
//
// Threads: everything here runs on the main (host) thread. The launcher draws straight to the
// window before the guest boots. In game the menu is composited into a copy of the present image
// (ComposeFrame) that the present thread or the inline present then shows, so the present thread
// (core/gxf12_presenter.h) is untouched. ImGui lays out in window coordinates; the draw data is
// mapped from the picture rectangle in the window onto the present image.
#include "ui_hooks.h"

#include "ui_crashreport.h" // UIT14 (windows.h with NOMINMAX, before glad defines APIENTRY)

#include <glad/glad.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <ctime>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h" // UIT14 autotest queries (nav state, read only)
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl2.h"

#include "../input/gamepad_motion.h"
#include "../input/input_bindings.h"
#include "input/controller_events.h"
#include "input/input_manager.h"
#include "runtime/config.h"
#include "runtime/cpu_context.h" // MMU (autotest guest32: query)
#include "runtime/ui_config.h"
#include "runtime/interp_config.h" // LAUNCH19: perf overlay (in-between frames on/off)
#include "ui_assets.h"
#include "ui_autotest.h"
#include "ui_capture.h"
#include "ui_controls_tab.h" // CTRL18-UI: Besturing tab, rebind capture, glyph set switch
#include "../input/game_context.h" // CTRL18-UI: g_port_menu_open (CTRL18-INPUT); path: CTRL18-CAM (lib include path)
#include "ui_disc.h"
#include "ui_font.h"
#include "ui_settings.h"
#include "ui_wic.h"
#include "ui_thp_player.h" // THPVID19: launcher background video
#include "ui_widgets.h"
#include "ui_settings_page.h"      // LAUNCH19: the SkyBreak settings page (one catalog drives every row)
#include "ui_saves.h"              // LAUNCH19: save backups at start, Dolphin import/export
#include "ui_achievements.h"
#include "runtime/discord_presence.h"
#include "runtime/mods.h"
#include "ui_diag.h"               // LAUNCH19: diagnostics zip
#include "../platform/audio_out.h" // LAUNCH19: master volume / focus mute / device + latency at game start
#include <SDL_syswm.h>             // LAUNCH19: the window handle for the folder dialog
#ifdef _WIN32
#include <shobjidl.h> // LAUNCH19: IFileOpenDialog (folder picker)
#ifdef small
#undef small // rpcndr.h: "#define small char" (page::Fonts::small)
#endif
#endif

// UIT14 autotest: SDL's own internal event senders (static SDL2: the same calls its Windows driver makes for
// real keyboard/mouse input, so key state, modifiers, focus and window IDs behave as for a player).
extern "C" {
int SDL_SendKeyboardKey(Uint8 state, SDL_Scancode scancode);
void SDL_SetKeyboardFocus(SDL_Window* window);
int SDL_SendMouseMotion(SDL_Window* window, Uint32 mouseID, int relative, int x, int y);
int SDL_SendMouseButton(SDL_Window* window, Uint32 mouseID, Uint8 state, Uint8 button);
}
extern uint64_t g_stat_frames; // hle/gx.cpp: game frames (autotest queries / marks)
namespace nwii::runtime {
extern MMU* g_mmu; // core/main.cpp: guest memory (autotest "guest32:" query, read only)
}

namespace nwii::ui {

namespace {
namespace fs = std::filesystem;
using runtime::Config;

constexpr const char* kVersion = "0.23.0 Alpha 3";

// ---------------------------------------------------------------------------------------- look
const ImU32 kGold = IM_COL32(236, 200, 120, 255);
const ImU32 kGoldBright = IM_COL32(255, 226, 150, 255);
const ImU32 kParchment = IM_COL32(244, 232, 205, 255);
const ImU32 kMuted = IM_COL32(190, 176, 146, 255);

struct Toast {
    std::string title, body;
    double t0 = 0;
};
enum class Page { Main, Settings, Achievements, Report, Mods };
enum class Action { None, Play, Quit };

struct UiState {
    bool imgui = false;          // context + backends initialised
    bool overlay = false;        // in-game menu active this session
    bool launcher_arg_skip = false;
    bool selftest = false;
    int force_w = 0, force_h = 0; // self-test: the hidden window's client size may differ
    SDL_Window* window = nullptr;
    std::string game_dir, config_path, settings_path;
    // look
    GLuint bg_tex = 0, logo_tex = 0;
    int bg_w = 0, bg_h = 0, logo_w = 0, logo_h = 0;
    BrfntFont game_font;
    bool have_game_font = false;
    ImFont *f_menu = nullptr, *f_caps = nullptr, *f_body = nullptr, *f_bold = nullptr, *f_small = nullptr,
           *f_head = nullptr;
    float font_scale = 0.0f;
    std::vector<PendingGlyph> pending;
    // disc check
    std::thread disc_thread;
    std::atomic<bool> disc_done{false};
    DiscStatus disc;
    // toasts / controllers
    std::deque<Toast> toasts;
    // settings editing
    Settings saved, edit;
    std::string settings_problem; // CTRL18-UI: a settings file with a TOML error (Dutch warning; "" = none)
    std::string status;
    bool status_error = false;
    int tab = 0;
    Page page = Page::Main;
    int menu_sel = 0;
    bool focus_menu = true;
    bool show_nav = false; // UIT14: make the nav cursor visible once the focused entry is known (ui_widgets.h)
    // in game
    bool in_game = false;
    std::atomic<bool> menu_open{false};
    bool confirm_quit = false;
    GLuint ui_fbo = 0, ui_tex = 0;
    int ui_w = 0, ui_h = 0;
    double last_compose = 0;
    long (__stdcall *dwm_flush)() = nullptr;
    // UIT14 autotest (NWII_UI_AUTOTEST): script runner, virtual cursor, widget rectangles of the last UI frame
    bool in_launcher = false;
    std::unique_ptr<autotest::Runner> at;
    bool at_cursor = false;
    std::string at_scroll_to; // CTRL18-UI: autotest "do scroll_to <label>"
    int at_scroll_steps = 0;  // CTRL18-UI
    float at_x = 0, at_y = 0;
    std::map<std::string, std::pair<ImVec2, ImVec2>> rects, rects_next;
    std::string shot_path;
    std::map<std::string, ImGuiID> ids; // label -> ImGui ID (autotest "nav" query)
    Uint32 dbg_key_win = 0;
    bool at_failed = false; // NWII_UI_AUTOTEST set but the script is unusable: never wait for a human
    double at_done_t = 0;
    bool at_auto_pc = false;                  // "do auto_point_center 1" (AutoPointCenter)
    double at_pc_since = 0, at_pc_release = 0;
    SDL_Keycode dbg_key = 0;
    // Launcher refresh: motion uses stable input rectangles, achievements are local player-wide.
    achievements::Store achievements;
    bool achievements_loaded = false;
    double achievement_retry = 0, page_since = 0;
    Page drawn_page = Page::Main;
    bool report_pending = false, report_save = false;
    char report_note[1024] = {};
    std::string report_path, report_error;
    // LAUNCH19
    page::State pst;              // the settings page (tab, focus per tab, dialogs, sub page)
    Settings boot;                // what the running game booted with (AtBoot rows: restart chips in F1)
    Settings boot_app;            // what this SkyBreak started with (Restart rows: VSync, window, threads)
    ImFont *f_tab = nullptr, *f_word = nullptr;
    GLuint wing_tex = 0;          // the SkyBreak wing (outputs/branding, embedded in the exe as RCDATA)
    int wing_w = 0, wing_h = 0;
    std::string renderer, log_dir, save_info, backup_msg;
    saves::Info save;
    std::vector<std::string> audio_devices;
    bool focused = true;          // the window has the input focus (game audio: mute when not active)
    bool font_game = true;        // the game font is in the current atlas
    // perf overlay ([ui] perf_overlay): presented frames counted in ComposeFrame
    double perf_t0 = 0, perf_last = 0;
    int perf_presents = 0;
    uint64_t perf_frames0 = 0;
    float perf_fps = 0, perf_game_fps = 0, perf_ms = 0, perf_ms_max = 0, perf_ms_acc = 0;
    // remember window ([ui] remember_window): the values at start, written back at exit when they changed
    int win_w0 = 0, win_h0 = 0, win_x0 = INT_MIN, win_y0 = INT_MIN;
    bool win_fs0 = false;
};
UiState g;

double Now() { return double(SDL_GetPerformanceCounter()) / double(SDL_GetPerformanceFrequency()); }
fs::path U8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string P8(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

fs::path ExeDir() {
    if (char* b = SDL_GetBasePath()) {
        const fs::path p = U8(b);
        SDL_free(b);
        return p.parent_path(); // SDL's base path ends with a separator
    }
    return fs::current_path();
}

std::string NandRoot() {
    if (const char* e = std::getenv("NWII_NAND_ROOT"); e && *e) return e;
    if (!Config::get().nand_root.empty()) return Config::get().nand_root;
    return P8((ExeDir().parent_path() / "nand").lexically_normal());
}

void OpenFolder(const std::string& path_utf8) {
    std::string url = "file:///" + path_utf8;
    std::replace(url.begin(), url.end(), '\\', '/');
    SDL_OpenURL(url.c_str());
}

std::string BuildDate() {
    // __DATE__ = "Mmm dd yyyy"
    static const char* en[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    static const char* nl[] = {"jan", "feb", "mrt", "apr", "mei", "jun", "jul", "aug", "sep", "okt", "nov", "dec"};
    const std::string d = __DATE__;
    std::string month = d.substr(0, 3);
    for (int i = 0; i < 12; ++i)
        if (month == en[i]) month = en[i];
    std::string day = d.substr(4, 2);
    if (!day.empty() && day[0] == ' ') day.erase(0, 1);
    return day + " " + month + " " + d.substr(7, 4) + " " + std::string(__TIME__).substr(0, 5);
}

GLuint Upload(const Image& im) {
    if (im.w <= 0 || im.h <= 0) return 0;
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, im.w, im.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, im.rgba.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    return t;
}

// LAUNCH19: our own SkyBreak wing (outputs/branding/skybreak-logo-512.png), embedded in the exe as RCDATA
// "SKYBREAK_LOGO" (src/ui/res/skybreak.rc). False when the exe has no such resource (test builds).
bool LoadEmbeddedLogo(Image& im) {
#ifdef _WIN32
    HMODULE mod = GetModuleHandleW(nullptr);
    HRSRC r = FindResourceW(mod, L"SKYBREAK_LOGO", MAKEINTRESOURCEW(10)); // RT_RCDATA
    if (!r) return false;
    HGLOBAL h = LoadResource(mod, r);
    const void* data = h ? LockResource(h) : nullptr;
    const DWORD size = SizeofResource(mod, r);
    if (!data || !size) return false;
    const std::vector<uint8_t> png(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
    std::string err;
    return DecodeImageWic(png, im, &err);
#else
    (void)im;
    return false;
#endif
}

// LAUNCH19: the window icon (title bar, taskbar, Alt+Tab) from the wing: a 64 px box-filtered copy.
void SetWindowIconFrom(const Image& im) {
    if (!g.window || im.w <= 0 || im.h <= 0) return;
    const int n = 64;
    std::vector<uint8_t> px(size_t(n) * n * 4, 0);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const int x0 = x * im.w / n, x1 = std::max(x0 + 1, (x + 1) * im.w / n);
            const int y0 = y * im.h / n, y1 = std::max(y0 + 1, (y + 1) * im.h / n);
            uint32_t acc[4] = {0, 0, 0, 0}, cnt = 0;
            for (int yy = y0; yy < y1; ++yy)
                for (int xx = x0; xx < x1; ++xx, ++cnt)
                    for (int c = 0; c < 4; ++c) acc[c] += im.rgba[(size_t(yy) * im.w + xx) * 4 + c];
            for (int c = 0; c < 4; ++c) px[(size_t(y) * n + x) * 4 + c] = uint8_t(acc[c] / std::max(1u, cnt));
        }
    if (SDL_Surface* s = SDL_CreateRGBSurfaceWithFormatFrom(px.data(), n, n, 32, n * 4, SDL_PIXELFORMAT_RGBA32)) {
        SDL_SetWindowIcon(g.window, s);
        SDL_FreeSurface(s);
    }
}

GLuint UploadMipmapped(const Image& im) {
    const GLuint t = Upload(im);
    if (!t) return 0;
    glBindTexture(GL_TEXTURE_2D, t);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);
    return t;
}

// The disc's own pictures and font. Every step is optional (fallbacks: gradient, text, Segoe UI).
void LoadLook() {
    const fs::path files = U8(g.game_dir) / "files";
    std::string err;
    { // LAUNCH19: SkyBreak's own identity + the GPU name for START / the diagnostics
        Image wing;
        if (LoadEmbeddedLogo(wing)) {
            g.wing_tex = UploadMipmapped(wing);
            g.wing_w = wing.w;
            g.wing_h = wing.h;
            SetWindowIconFrom(wing);
        } else {
            std::cout << "[LAUNCH19] no embedded SkyBreak logo (resource SKYBREAK_LOGO missing)" << std::endl;
        }
        const char* vendor = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        const char* ver = reinterpret_cast<const char*>(glGetString(GL_VERSION));
        std::string v = ver ? ver : "";
        if (v.size() > 3) v = v.substr(0, 3);
        g.renderer = "OpenGL " + v + "  \xC2\xB7  " + (vendor ? vendor : "?");
    }
    {
        std::string spec = runtime::ui_config().background;
        if (const char* e = std::getenv("NWII_UI_BACKGROUND"); e && *e) spec = e;
        if (spec.empty()) spec = "Demo01_02.thp:400";
        const size_t colon = spec.rfind(':');
        const std::string file = colon == std::string::npos ? spec : spec.substr(0, colon);
        const int frame = colon == std::string::npos ? 0 : std::atoi(spec.c_str() + colon + 1);
        std::vector<uint8_t> jpeg;
        Image im;
        ThpInfo info;
        if (ThpFrameJpeg(P8(files / "THP" / U8(file)), frame, jpeg, &info, &err) && DecodeImageWic(jpeg, im, &err)) {
            g.bg_tex = Upload(im);
            g.bg_w = im.w;
            g.bg_h = im.h;
            std::cout << "[UI13] background: " << file << " frame " << frame << " (" << im.w << "x" << im.h << ")\n";
        } else {
            std::cout << "[UI13] background unavailable (" << err << "): plain backdrop\n";
        }
    }
    for (const char* region : {"US", "EU", "JP"}) {
        std::vector<uint8_t> arc;
        if (!ReadFile(P8(files / region / "Layout" / "Title2D.arc"), arc)) continue;
        std::vector<U8Entry> entries;
        if (!ParseU8(arc, entries, &err)) break;
        if (const U8Entry* e = FindU8(entries, "tr_wiiKing2Logo_00.tpl")) {
            Image im;
            if (DecodeTpl(arc.data() + e->offset, e->size, im, &err)) {
                g.logo_tex = Upload(im);
                g.logo_w = im.w;
                g.logo_h = im.h;
                std::cout << "[UI13] logo: " << region << "/Layout/Title2D.arc (" << im.w << "x" << im.h << ")\n";
            }
        }
        break;
    }
    { // LAUNCH19: loaded even with [ui] game_font = false (the setting switches it live)
        std::vector<uint8_t> font;
        if (ReadFile(P8(files / "US" / "Font" / "en_US" / "normal_00.brfnt"), font) &&
            ParseBrfnt(font, g.game_font, &err)) {
            g.have_game_font = true;
            std::cout << "[UI13] game font: normal_00.brfnt (" << g.game_font.glyphs.size() << " glyphs)\n";
        } else {
            std::cout << "[UI13] game font unavailable (" << err << "): Segoe UI\n";
        }
    }
}

// ---------------------------------------------------------------------------------------- fonts
ImFont* AddBrfnt(float k, const char* name) {
    return AddBrfntFont(ImGui::GetIO().Fonts, g.game_font, k, name, g.pending);
}

ImFont* AddTtf(const char* file, float px, const ImWchar* ranges) {
    ImGuiIO& io = ImGui::GetIO();
    std::string dir = "C:\\Windows";
    if (const char* w = std::getenv("WINDIR"); w && *w) dir = w;
    const std::string path = dir + "\\Fonts\\" + file;
    std::error_code ec;
    if (fs::is_regular_file(U8(path), ec)) {
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        cfg.OversampleV = 1;
        if (ImFont* f = io.Fonts->AddFontFromFileTTF(path.c_str(), px, &cfg, ranges)) return f;
    }
    ImFontConfig cfg;
    cfg.SizePixels = std::round(px * 0.8f);
    return io.Fonts->AddFontDefault(&cfg);
}

void BuildFonts(float s) {
    ImGuiIO& io = ImGui::GetIO();
    static const ImWchar kRanges[] = {0x0020, 0x00FF, 0x2013, 0x2014, 0x2018, 0x201D, 0x2022, 0x2022,
                                      0x2026, 0x2026, 0x2190, 0x2193, 0};
    io.Fonts->Clear();
    g.pending.clear();
    g.f_body = AddTtf("segoeui.ttf", std::round(19.0f * s), kRanges); // first = default font
    g.f_bold = AddTtf("seguisb.ttf", std::round(19.0f * s), kRanges);
    g.f_small = AddTtf("segoeui.ttf", std::round(15.0f * s), kRanges);
    g.f_head = AddTtf("seguisb.ttf", std::round(24.0f * s), kRanges);
    g.font_game = g.have_game_font && runtime::ui_config().game_font; // LAUNCH19: [ui] game_font (live)
    if (g.font_game) {
        g.f_menu = AddBrfnt(1.05f * s, "SS normal_00 menu");
        g.f_caps = AddBrfnt(0.52f * s, "SS normal_00 small");
        g.f_tab = AddBrfnt(0.66f * s, "SS normal_00 tab");   // LAUNCH19: settings tabs
        g.f_word = AddBrfnt(1.9f * s, "SS normal_00 word");  // LAUNCH19: the SkyBreak wordmark
    } else {
        g.f_menu = AddTtf("seguisb.ttf", std::round(36.0f * s), kRanges);
        g.f_caps = AddTtf("seguisb.ttf", std::round(15.0f * s), kRanges);
        g.f_tab = AddTtf("seguisb.ttf", std::round(19.0f * s), kRanges); // LAUNCH19
        g.f_word = AddTtf("seguisb.ttf", std::round(58.0f * s), kRanges); // LAUNCH19
    }
    io.Fonts->Build();
    FinishBrfntGlyphs(io.Fonts, g.pending);
    g.font_scale = s;
}

void ApplyStyle(float s) {
    ImGuiStyle st;
    ImGui::StyleColorsDark(&st);
    st.WindowRounding = 8; st.ChildRounding = 6; st.FrameRounding = 5; st.PopupRounding = 6;
    st.GrabRounding = 4; st.TabRounding = 5; st.ScrollbarRounding = 6;
    st.WindowBorderSize = 1; st.FrameBorderSize = 0; st.PopupBorderSize = 1;
    st.WindowPadding = ImVec2(20, 16); st.FramePadding = ImVec2(10, 6); st.ItemSpacing = ImVec2(10, 10);
    st.ItemInnerSpacing = ImVec2(8, 6); st.ScrollbarSize = 14; st.GrabMinSize = 14;
    ImVec4* c = st.Colors;
    const ImVec4 gold(0.925f, 0.784f, 0.470f, 1.0f);
    c[ImGuiCol_Text] = ImVec4(0.957f, 0.910f, 0.804f, 1.0f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.62f, 0.56f, 0.45f, 1.0f);
    c[ImGuiCol_WindowBg] = ImVec4(0.070f, 0.056f, 0.038f, 0.93f);
    c[ImGuiCol_ChildBg] = ImVec4(0.11f, 0.088f, 0.058f, 0.60f);
    c[ImGuiCol_PopupBg] = ImVec4(0.09f, 0.072f, 0.048f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(gold.x, gold.y, gold.z, 0.45f);
    c[ImGuiCol_FrameBg] = ImVec4(0.21f, 0.165f, 0.10f, 0.90f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.31f, 0.245f, 0.14f, 0.95f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.39f, 0.30f, 0.16f, 1.0f);
    c[ImGuiCol_CheckMark] = gold;
    c[ImGuiCol_SliderGrab] = ImVec4(0.85f, 0.70f, 0.40f, 1.0f);
    c[ImGuiCol_SliderGrabActive] = ImVec4(1.0f, 0.86f, 0.52f, 1.0f);
    c[ImGuiCol_Button] = ImVec4(0.27f, 0.205f, 0.12f, 0.95f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.47f, 0.36f, 0.18f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.62f, 0.48f, 0.22f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(gold.x, gold.y, gold.z, 0.22f);
    c[ImGuiCol_HeaderHovered] = ImVec4(gold.x, gold.y, gold.z, 0.32f);
    c[ImGuiCol_HeaderActive] = ImVec4(gold.x, gold.y, gold.z, 0.45f);
    c[ImGuiCol_Separator] = ImVec4(gold.x, gold.y, gold.z, 0.35f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0.20f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.55f, 0.45f, 0.27f, 0.8f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.70f, 0.57f, 0.32f, 0.9f);
    c[ImGuiCol_ScrollbarGrabActive] = gold;
    c[ImGuiCol_NavCursor] = gold;
    c[ImGuiCol_TableHeaderBg] = ImVec4(0.20f, 0.16f, 0.10f, 1.0f);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.03f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.55f);
    st.ScaleAllSizes(s);
    ImGui::GetStyle() = st;
}

float UiScale(float w, float h) {
    const float fit = std::clamp(std::min(h / 720.0f, w / 1280.0f), 0.75f, 3.0f);
    const int pct = runtime::ui_config().scale; // LAUNCH19: [ui] scale (0 = automatic, else percent)
    if (pct <= 0) return fit;
    return std::clamp(pct / 100.0f, 0.5f, std::max(0.5f, std::min(3.0f, std::min(w / 960.0f, h / 540.0f))));
}

void EnsureFonts(float s) {
    if (std::fabs(s - g.font_scale) < 0.06f && g.font_game == (g.have_game_font && runtime::ui_config().game_font)) return;
    BuildFonts(s);
    ApplyStyle(s);
    ImGui_ImplOpenGL3_DestroyFontsTexture();
    ImGui_ImplOpenGL3_CreateFontsTexture();
}

// ---------------------------------------------------------------------------------------- toasts
void PushToast(const std::string& title, const std::string& body) {
    if (!runtime::ui_config().toasts) return;
    g.toasts.push_back({title, body, Now()});
    while (g.toasts.size() > 3) g.toasts.pop_front();
}

// INPUT13 owns every SDL joystick call (controller thread); the UI never calls one (a hung HID driver
// would freeze the window). Controller news comes from its thread-safe queue (controller_events.h).
void DrainControllerEvents() {
    using runtime::input::ControllerEventKind;
    runtime::input::ControllerEvent e;
    while (runtime::input::poll_controller_event(e)) {
        const std::string name = e.name[0] ? e.name : "Controller";
        const bool en = runtime::ui_config().language == "en"; // REVLAUNCH19: the notices follow [ui] language too
        const auto T = [en](const char* nl, const char* e2) { return en ? e2 : nl; };
        switch (e.kind) {
        case ControllerEventKind::Connected: PushToast(T("CONTROLLER VERBONDEN", "CONTROLLER CONNECTED"), name); break;
        case ControllerEventKind::Disconnected: PushToast(T("CONTROLLER LOSGEKOPPELD", "CONTROLLER DISCONNECTED"), name); break;
        case ControllerEventKind::DetectionBlocked:
            PushToast(T("CONTROLLERS REAGEREN NIET", "CONTROLLERS NOT RESPONDING"),
                      T("Toetsenbord en muis werken gewoon door", "Keyboard and mouse keep working"));
            break;
        case ControllerEventKind::DetectionRecovered: PushToast(T("CONTROLLERS WEER BESCHIKBAAR", "CONTROLLERS BACK"), name); break;
        case ControllerEventKind::GyroOn: PushToast(T("GYRO AAN", "GYRO ON"), name); break;
        case ControllerEventKind::GyroOff: PushToast(T("GYRO UIT", "GYRO OFF"), name); break;
        case ControllerEventKind::GyroRecentred: PushToast(T("WIJZER GECENTREERD", "POINTER CENTRED"), name); break;
        }
    }
}

// Controller buttons/sticks for ImGui's gamepad navigation, from the SDL events the controller thread
// posts (no SDL joystick call here).
// LAUNCH19: face buttons BY POSITION with CTRL18-INPUT's pad-type table (face_button_for): ImGui FaceDown / Right /
// Left / Up are the south / east / west / north buttons on every pad (SDL reports Nintendo pads by label: their "A"
// is the east button). io.ConfigNavSwapGamepadButtons (NewUiFrame) makes east confirm (Switch layout, user decision)
// unless [ui] confirm_button = "south". Triggers = L2/R2 (page), right stick = ImGui's right-stick keys.
bool NintendoPad() {
    runtime::input::ControllerStatus st[4];
    runtime::input::controller_status(st);
    for (const auto& c : st)
        if (c.connected) return std::strstr(c.type, "Switch") || std::strstr(c.type, "Joy-Con");
    return false;
}
void FeedGamepad(const SDL_Event& e) {
    ImGuiIO& io = ImGui::GetIO();
    if (e.type == SDL_CONTROLLERBUTTONDOWN || e.type == SDL_CONTROLLERBUTTONUP) {
        const bool down = e.type == SDL_CONTROLLERBUTTONDOWN;
        ImGuiKey k = ImGuiKey_None;
        const int b = e.cbutton.button;
        if (b == SDL_CONTROLLER_BUTTON_A || b == SDL_CONTROLLER_BUTTON_B || b == SDL_CONTROLLER_BUTTON_X ||
            b == SDL_CONTROLLER_BUTTON_Y) {
            static const ImGuiKey kPos[4] = {ImGuiKey_GamepadFaceDown, ImGuiKey_GamepadFaceRight, ImGuiKey_GamepadFaceLeft,
                                             ImGuiKey_GamepadFaceUp};
            const int type = (e.cbutton.which >= 0 && NintendoPad()) ? 5 : 0; // autotest pads (which -1): generic
            for (int pos = 0; pos < 4; ++pos)
                if (runtime::input::face_button_for(pos, type) == b) k = kPos[pos];
        }
        switch (b) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP: k = ImGuiKey_GamepadDpadUp; break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: k = ImGuiKey_GamepadDpadDown; break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: k = ImGuiKey_GamepadDpadLeft; break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: k = ImGuiKey_GamepadDpadRight; break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: k = ImGuiKey_GamepadL1; break;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: k = ImGuiKey_GamepadR1; break;
        case SDL_CONTROLLER_BUTTON_START: k = ImGuiKey_GamepadStart; break;
        case SDL_CONTROLLER_BUTTON_BACK: k = ImGuiKey_GamepadBack; break;
        default: break;
        }
        if (k != ImGuiKey_None) io.AddKeyEvent(k, down);
    } else if (e.type == SDL_CONTROLLERAXISMOTION) {
        const float v = e.caxis.value / 32767.0f;
        const auto pair = [&](ImGuiKey neg, ImGuiKey pos) {
            io.AddKeyAnalogEvent(neg, v < -0.30f, v < -0.30f ? std::min(1.0f, (-v - 0.30f) / 0.70f) : 0.0f);
            io.AddKeyAnalogEvent(pos, v > 0.30f, v > 0.30f ? std::min(1.0f, (v - 0.30f) / 0.70f) : 0.0f);
        };
        switch (e.caxis.axis) {
        case SDL_CONTROLLER_AXIS_LEFTX: pair(ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight); break;
        case SDL_CONTROLLER_AXIS_LEFTY: pair(ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown); break;
        case SDL_CONTROLLER_AXIS_RIGHTX: pair(ImGuiKey_GamepadRStickLeft, ImGuiKey_GamepadRStickRight); break; // LAUNCH19
        case SDL_CONTROLLER_AXIS_RIGHTY: pair(ImGuiKey_GamepadRStickUp, ImGuiKey_GamepadRStickDown); break;    // LAUNCH19
        case SDL_CONTROLLER_AXIS_TRIGGERLEFT: io.AddKeyAnalogEvent(ImGuiKey_GamepadL2, v > 0.5f, std::max(0.0f, v)); break;
        case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: io.AddKeyAnalogEvent(ImGuiKey_GamepadR2, v > 0.5f, std::max(0.0f, v)); break;
        default: break;
        }
    }
}

void DrawPadIcon(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
    const float w = 30 * s, h = 18 * s;
    dl->AddRectFilled(ImVec2(c.x - w / 2, c.y - h / 2), ImVec2(c.x + w / 2, c.y + h / 2), col, 8 * s);
    dl->AddCircleFilled(ImVec2(c.x - w / 2 + 3 * s, c.y + h / 2 - 2 * s), 6 * s, col);
    dl->AddCircleFilled(ImVec2(c.x + w / 2 - 3 * s, c.y + h / 2 - 2 * s), 6 * s, col);
    const ImU32 dark = IM_COL32(28, 22, 14, 255);
    dl->AddRectFilled(ImVec2(c.x - 10 * s, c.y - 1.2f * s), ImVec2(c.x - 4 * s, c.y + 1.2f * s), dark);
    dl->AddRectFilled(ImVec2(c.x - 8.2f * s, c.y - 3 * s), ImVec2(c.x - 5.8f * s, c.y + 3 * s), dark);
    dl->AddCircleFilled(ImVec2(c.x + 6 * s, c.y - 2 * s), 1.6f * s, dark);
    dl->AddCircleFilled(ImVec2(c.x + 9 * s, c.y + 1 * s), 1.6f * s, dark);
}

// Toasts stacked from the top-right corner of `area` (min, max).
void DrawToasts(ImDrawList* dl, ImVec2 amin, ImVec2 amax, float s) {
    const double now = Now();
    while (!g.toasts.empty() && now - g.toasts.front().t0 > 4.2) g.toasts.pop_front();
    float y = amin.y + 22 * s;
    for (const Toast& t : g.toasts) {
        const double age = now - t.t0;
        const float a = float(std::clamp(std::min(age / 0.25, (4.2 - age) / 0.5), 0.0, 1.0));
        const float w = 390 * s, h = 66 * s;
        const float slide = (1.0f - a) * 24 * s;
        const ImVec2 p0(amax.x - 22 * s - w + slide, y), p1(amax.x - 22 * s + slide, y + h);
        const auto A = [&](ImU32 c) { return (c & 0x00FFFFFF) | (ImU32(((c >> 24) & 255) * a) << 24); };
        dl->AddRectFilled(p0, p1, A(IM_COL32(16, 12, 8, 225)), 8 * s);
        dl->AddRect(p0, p1, A(IM_COL32(236, 200, 120, 120)), 8 * s, 0, 1.0f * s);
        dl->AddRectFilled(ImVec2(p0.x, p0.y + 10 * s), ImVec2(p0.x + 3 * s, p1.y - 10 * s), A(kGold));
        if (t.title.rfind("ACHIEVEMENT",0)==0) {
            const float x=p0.x+36*s, y=(p0.y+p1.y)/2;
            dl->AddQuadFilled(ImVec2(x-13*s,y-12*s),ImVec2(x+13*s,y-12*s),ImVec2(x+8*s,y+5*s),ImVec2(x-8*s,y+5*s),A(kGold));
            dl->AddLine(ImVec2(x,y+5*s),ImVec2(x,y+14*s),A(kGold),3*s);
            dl->AddLine(ImVec2(x-9*s,y+15*s),ImVec2(x+9*s,y+15*s),A(kGold),3*s);
            dl->AddCircle(ImVec2(x-12*s,y-5*s),7*s,A(kGold),16,2*s);
            dl->AddCircle(ImVec2(x+12*s,y-5*s),7*s,A(kGold),16,2*s);
        } else DrawPadIcon(dl, ImVec2(p0.x + 36 * s, (p0.y + p1.y) / 2), s, A(kGold));
        dl->AddText(g.f_caps, g.f_caps->FontSize, ImVec2(p0.x + 66 * s, p0.y + 10 * s), A(kGold), t.title.c_str());
        dl->AddText(g.f_bold, g.f_bold->FontSize, ImVec2(p0.x + 66 * s, p0.y + 32 * s), A(kParchment), t.body.c_str());
        y += h + 10 * s;
    }
}

// ---------------------------------------------------------------------------------------- widgets
// UIT14: the autotest clicks widgets by label; only recorded while a script runs.
void TrackRect(const std::string& label, ImVec2 a, ImVec2 b) {
    if (g.at) g.rects_next[label] = {a, b};
}
void Track(const std::string& label) {
    if (!g.at) return;
    TrackRect(label, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    g.ids[label] = ImGui::GetItemID();
    if (!g.at_scroll_to.empty() && label == g.at_scroll_to) { // CTRL18-UI: "do scroll_to <label>" (long pages)
        ImGui::SetScrollHereY(0.5f);
        g.at_scroll_to.clear();
    }
}

// ImGui::Combo with every entry tracked as "<label>/<entry>" (the box itself as "<label>").
bool ComboT(const char* label, int* v, const char* const items[], int n) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    TrackRect(label, p, ImVec2(p.x + ImGui::CalcItemWidth(), p.y + ImGui::GetFrameHeight()));
    bool changed = false;
    if (ImGui::BeginCombo(label, *v >= 0 && *v < n ? items[*v] : "")) {
        for (int i = 0; i < n; ++i) {
            const bool sel = *v == i;
            if (ImGui::Selectable(items[i], sel)) {
                *v = i;
                changed = true;
            }
            if (sel) ImGui::SetItemDefaultFocus();
            Track(std::string(label) + "/" + items[i]);
        }
        ImGui::EndCombo();
    }
    return changed;
}

// A big menu entry in the game font: gold glow + diamond when hovered/focused or selected.
bool MenuEntry(const char* id, const char* label, int index, float s, float min_w = 0.0f) {
    ImFont* f = g.f_menu;
    const ImVec2 ts = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, label);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 sz(std::max(ts.x + 80 * s, min_w > 0 ? min_w : 400 * s), f->FontSize + 12 * s);
    ImGui::PushStyleColor(ImGuiCol_NavCursor, IM_COL32(0, 0, 0, 0));
    bool selected = false;
    const bool pressed = MenuHitArea(id, sz, &g.show_nav, &selected); // UIT14: keyboard/controller navigable
    ImGui::PopStyleColor();
    Track(label);
    if (selected) g.menu_sel = index; // REVUIT14: one selected entry (was hovered || focused: two lit up)
    const bool hot = g.menu_sel == index;
    const ImGuiID key = ImGui::GetItemID();
    ImGuiStorage* storage = ImGui::GetStateStorage();
    float glow = storage->GetFloat(key, 0.0f);
    const float target = hot ? 1.0f : 0.0f;
    glow += (target - glow) * (1.0f - std::exp(-16.0f * ImGui::GetIO().DeltaTime));
    storage->SetFloat(key, glow);
    float push = storage->GetFloat(key ^ 0x412aba, 0.0f);
    push += ((ImGui::IsItemActive() ? 1.0f : 0.0f) - push) * (1.0f - std::exp(-24.0f * ImGui::GetIO().DeltaTime));
    storage->SetFloat(key ^ 0x412aba, push);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cy = pos.y + sz.y * 0.5f, slide = (8 * glow + 3 * push) * s;
    dl->AddRectFilled(pos, ImVec2(pos.x + sz.x, pos.y + sz.y), IM_COL32(7, 20, 30, 65), 5 * s);
    dl->AddRectFilledMultiColor(pos, ImVec2(pos.x + sz.x, pos.y + sz.y), IM_COL32(236, 200, 120, int(100 * glow)),
                                IM_COL32(236, 200, 120, 0), IM_COL32(236, 200, 120, 0), IM_COL32(236, 200, 120, int(100 * glow)));
    dl->AddLine(ImVec2(pos.x, pos.y + sz.y), ImVec2(pos.x + sz.x * (0.25f + 0.55f * glow), pos.y + sz.y),
                IM_COL32(236, 200, 120, int(30 + 130 * glow)), s);
    const float d = (4 + 3 * glow) * s, x = pos.x + 16 * s + slide;
    dl->AddQuadFilled(ImVec2(x, cy - d), ImVec2(x + d, cy), ImVec2(x, cy + d), ImVec2(x - d, cy),
                      IM_COL32(255, 226, 150, int(55 + 200 * glow)));
    const ImVec4 cold = ImGui::ColorConvertU32ToFloat4(kParchment), warm = ImGui::ColorConvertU32ToFloat4(kGoldBright);
    const ImU32 text = ImGui::ColorConvertFloat4ToU32(ImVec4(cold.x+(warm.x-cold.x)*glow, cold.y+(warm.y-cold.y)*glow,
                                                          cold.z+(warm.z-cold.z)*glow, 1));
    dl->AddText(f, f->FontSize, ImVec2(pos.x + 36 * s + slide, pos.y + 6 * s), text, label);
    return pressed;
}

void SectionTitle(const char* text) {
    ImGui::PushFont(g.f_head);
    ImGui::TextColored(ImVec4(0.925f, 0.784f, 0.470f, 1.0f), "%s", text);
    ImGui::PopFont();
}

void Hint(const char* text) {
    ImGui::PushFont(g.f_small);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void RestartBadge(bool show) {
    if (!show) return;
    ImGui::SameLine();
    ImGui::PushFont(g.f_small);
    ImGui::TextColored(ImVec4(0.95f, 0.66f, 0.35f, 1.0f), "(na herstart)");
    ImGui::PopFont();
}

// Dutch labels for the INPUT10 actions (input_bindings.h names).
const char* ActionLabel(const std::string& n, const char* fallback) {
    static const std::map<std::string, const char*> kNl = {
        {"a", "Wii A"}, {"b", "Wii B"}, {"1", "Wii 1"}, {"2", "Wii 2"}, {"plus", "Wii +"}, {"minus", "Wii -"},
        {"home", "Wii Home"}, {"up", "Kruis omhoog"}, {"down", "Kruis omlaag"}, {"left", "Kruis links"},
        {"right", "Kruis rechts"}, {"c", "Nunchuk C"}, {"z", "Nunchuk Z"}, {"stick_up", "Nunchuk-stick omhoog"},
        {"stick_down", "Nunchuk-stick omlaag"}, {"stick_left", "Nunchuk-stick links"},
        {"stick_right", "Nunchuk-stick rechts"}, {"swing_up", "Zwaardslag omhoog"}, {"swing_down", "Zwaardslag omlaag"},
        {"swing_left", "Zwaardslag links"}, {"swing_right", "Zwaardslag rechts"}, {"swing_up_left", "Slag linksboven"},
        {"swing_up_right", "Slag rechtsboven"}, {"swing_down_left", "Slag linksonder"},
        {"swing_down_right", "Slag rechtsonder"}, {"thrust", "Steek"}, {"raise", "Zwaard heffen (vasthouden)"},
        {"nunchuk_flick", "Nunchuk-zwiep (schild)"}, {"spin_horizontal", "Draaiaanval horizontaal"},
        {"spin_vertical", "Draaiaanval verticaal"}, {"mouse_swing", "Vasthouden + muis zwiepen = slag"},
        {"roll_left", "Remote naar links kantelen"}, {"roll_right", "Remote naar rechts kantelen"},
        {"aim_relative", "Vasthouden: muis draait de remote"}, {"spin_modifier", "Met stick-zwiep: draaiaanval"},
        {"nunchuk_shake", "Nunchuk schudden"}, {"recenter", "Wijzer centreren"},
        {"gyro_toggle", "1 s vasthouden: gyro aan/uit"}};
    auto it = kNl.find(n);
    return it != kNl.end() ? it->second : fallback;
}

std::string JoinNames(const std::vector<const char*>& d) {
    std::string s;
    for (const char* x : d) s += (s.empty() ? "" : ", ") + std::string(x);
    return s;
}
std::string JoinNames(const std::vector<std::string>& d) {
    std::string s;
    for (const auto& x : d) s += (s.empty() ? "" : ", ") + x;
    return s.empty() ? std::string("(geen)") : s;
}

void BindingsTable() {
    using namespace runtime::input;
    const Config& cfg = Config::get();
    struct Row { std::string name; const char* help; std::string key, pad; };
    std::vector<Row> rows;
    auto find = [&](const std::string& n) -> Row* {
        for (Row& r : rows) if (r.name == n) return &r;
        return nullptr;
    };
    for (const ActionInfo& a : keyboard_actions()) rows.push_back({a.name, a.help, JoinNames(a.defaults), "-"});
    for (const ActionInfo& a : gamepad_actions()) {
        if (Row* r = find(a.name)) r->pad = JoinNames(a.defaults);
        else rows.push_back({a.name, a.help, "-", JoinNames(a.defaults)});
    }
    for (const auto& [n, list] : cfg.keyboard_bindings) if (Row* r = find(n)) r->key = JoinNames(list);
    for (const auto& [n, list] : cfg.gamepad_bindings) if (Row* r = find(n)) r->pad = JoinNames(list);
    ImGui::PushFont(g.f_small);
    if (ImGui::BeginTable("##bind", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Actie", ImGuiTableColumnFlags_WidthStretch, 1.3f);
        ImGui::TableSetupColumn("Toetsenbord / muis", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Controller", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableHeadersRow();
        for (const Row& r : rows) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(ActionLabel(r.name, r.help));
            ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(r.key.c_str());
            ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(r.pad.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::PopFont();
}

// ---------------------------------------------------------------------------------------- settings (LAUNCH19)
// The settings page itself is src/ui/ui_settings_page.cpp (one catalog drives every row); this part gives it what it
// needs (looks, facts, saving, applying, folder picking, the save/diagnostics actions).
bool ConfirmSouth() { return runtime::ui_config().confirm_button == "south"; }
// "Back" BY POSITION (FeedGamepad feeds positions): the south button with the Switch layout, east with "south".
ImGuiKey BackKey() { return ConfirmSouth() ? ImGuiKey_GamepadFaceRight : ImGuiKey_GamepadFaceDown; }
bool En() { return runtime::ui_config().language == "en"; }
const char* Tr(const char* nl, const char* en) { return En() ? en : nl; }
ImU32 AccentColour() {
    const std::string& a = runtime::ui_config().accent;
    if (a == "red") return IM_COL32(226, 86, 58, 255);
    if (a == "sky") return IM_COL32(110, 170, 230, 255);
    return IM_COL32(236, 200, 120, 255);
}

// Master volume x "mute when SkyBreak is not active" -> the host audio output (game only: in the launcher the
// background music has its own volume and focus mute).
void ApplyAudioGain() {
    if (!g.in_game) return;
    const runtime::UiConfig& u = runtime::ui_config();
    const bool mute = u.mute_when_unfocused && !g.focused;
    runtime::audio::set_output_gain(mute ? 0.0f : std::clamp(u.master_volume, 0, 100) / 100.0f);
}

// The rows that apply at once ("Werkt meteen") reach the running SkyBreak while the page is open (not saved).
void ApplyLive(const Settings& e) {
    Settings cur = FromConfig(Config::get());
    // REVLAUNCH19: the window / gyro mode only follow a CHANGE of their setting (as UI13 did on save). F11 / Alt+Enter and
    // the in-game gyro hotkey change the state without the setting; a preview of any other row must not undo that.
    const bool fs_changed = cur.fullscreen != e.fullscreen, gyro_changed = cur.gyro != e.gyro;
    for (const catalog::Desc& d : catalog::All())
        if (catalog::IsValueRow(d) && d.apply == catalog::Apply::Live) {
            const catalog::Ref from = catalog::RefOf(d, e), to = d.ref(cur);
            if (auto* p = std::get_if<bool*>(&from)) **std::get_if<bool*>(&to) = **p;
            else if (auto* p = std::get_if<int*>(&from)) **std::get_if<int*>(&to) = **p;
            else if (auto* p = std::get_if<float*>(&from)) **std::get_if<float*>(&to) = **p;
            else if (auto* p = std::get_if<std::string*>(&from)) **std::get_if<std::string*>(&to) = **p;
        }
    cur.bind_pad_hd = e.bind_pad_hd;
    cur.bind_pad_classic = e.bind_pad_classic;
    cur.bind_kbm = e.bind_kbm;
    cur.bind_kb_classic = e.bind_kb_classic;
    const bool font_change = runtime::ui_config().game_font != e.game_font;
    ToConfig(cur, Config::get());
    if (g.window && !g.selftest && fs_changed) { // REVLAUNCH19: fs_changed
        const bool fs_now = (SDL_GetWindowFlags(g.window) & SDL_WINDOW_FULLSCREEN) != 0;
        if (fs_now != e.fullscreen) SDL_SetWindowFullscreen(g.window, e.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    }
    if (g.in_game && gyro_changed) runtime::input::g_gamepad_motion_mode.store(e.gyro ? 1 : 0); // REVLAUNCH19: gyro_changed
    runtime::input::request_input_bindings_reload(); // presets / rebinds (CTRL18-INPUT)
    ApplyGlyphSet(e);                                // knopiconen
    ApplyAudioGain();
    if (font_change) g.font_scale = 0.0f;            // [ui] game_font: rebuild the fonts next frame
}

// Capture edits happen in the event loop, before the settings page snapshots
// its state for drawing. Publish them here so they preview immediately. Releases
// still reach ImGui, preventing the button that opened capture from staying held.
bool ProcessControlsEvent(const SDL_Event& e) {
    bool changed = false;
    const bool consumed = ControlsEvent(e, g.edit, g.saved, &changed);
    if (changed) ApplyLive(g.edit);
    if (consumed && e.type == SDL_CONTROLLERBUTTONUP) FeedGamepad(e);
    if (consumed && (e.type == SDL_KEYUP || e.type == SDL_MOUSEBUTTONUP)) ImGui_ImplSDL2_ProcessEvent(&e);
    return consumed;
}

bool SaveSettings(std::string& status) {
    const bool en = En();
    const auto edits = DiffEdits(g.saved, g.edit);
    if (edits.empty() && g.settings_problem.empty()) {
        status = Tr("Geen wijzigingen.", "No changes.");
        return true;
    }
    // CTRL18-UI: a settings file with a TOML error is kept as <file>.bak and rewritten with every current value.
    const SaveOutcome so = SaveUserSettings(g.settings_path, g.saved, g.edit);
    if (!so.ok) {
        status = std::string(Tr("Opslaan mislukt: ", "Saving failed: ")) + so.error;
        return false;
    }
    ToConfig(g.edit, Config::get());
    ApplyEnvFromConfig(Config::get());
    crashreport::ArmIfRequested(); // LAUNCH19: [diag] crash_report
    ApplyLive(g.edit);
    g.saved = g.edit;
    status = std::string(Tr("Opgeslagen (", "Saved (")) + std::to_string(so.keys) +
             (so.keys == 1 ? Tr(" instelling).", " setting).") : Tr(" instellingen).", " settings)."));
    if (!so.backup.empty()) { // CTRL18-UI
        status += std::string(Tr(" Het oude bestand had een fout en is bewaard als ", " The old file had an error and was kept as ")) +
                  so.backup + ".";
        g.settings_problem.clear();
    }
    RestartInfo ri = NeedsRestart(g.boot_app, g.edit, false, en); // Restart rows against SkyBreak's start
    if (g.in_game)                                                 // AtBoot rows against the game's boot
        for (const catalog::Desc& d : catalog::All())
            if (d.apply == catalog::Apply::AtBoot && catalog::IsValueRow(d) && !catalog::SameValue(d, g.boot, g.edit)) {
                ri.any = true;
                ri.what.push_back(catalog::T(d.label, en));
            }
    if (ri.any) {
        status += Tr(" Na een herstart actief: ", " Active after a restart: ");
        for (size_t i = 0; i < ri.what.size(); ++i) status += (i ? ", " : "") + ri.what[i];
        status += ".";
    }
    g.status = status;
    std::cout << "[UI13] settings saved to " << g.settings_path << " (" << edits.size() << " keys)\n";
    return true;
}
void RevertEdits() {
    g.edit = g.saved;
    ApplyLive(g.saved);
}

#ifdef _WIN32
std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}
std::string Narrow(const wchar_t* w) {
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(std::max(0, n - 1)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}
#endif

// The Windows folder dialog (IFileOpenDialog + FOS_PICKFOLDERS), modal on the SkyBreak window. "" = cancelled.
std::string PickFolder(const std::string& title, const std::string& start) {
    if (g.selftest || g.at) { // autotest / self-test: never a real dialog (NWII_UI_PICK_FOLDER answers it)
        const char* e = std::getenv("NWII_UI_PICK_FOLDER");
        std::cout << "[LAUNCH19] folder picker '" << title << "' -> " << (e ? e : "(cancelled)") << std::endl;
        return e ? e : "";
    }
#ifdef _WIN32
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::string out;
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
        DWORD opt = 0;
        dlg->GetOptions(&opt);
        dlg->SetOptions(opt | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dlg->SetTitle(Wide(title).c_str());
        if (!start.empty()) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(Wide(start).c_str(), nullptr, IID_PPV_ARGS(&item)))) {
                dlg->SetFolder(item);
                item->Release();
            }
        }
        HWND owner = nullptr;
        SDL_SysWMinfo wm;
        SDL_VERSION(&wm.version);
        if (g.window && SDL_GetWindowWMInfo(g.window, &wm)) owner = wm.info.win.window;
        if (SUCCEEDED(dlg->Show(owner))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR p = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                    out = Narrow(p);
                    CoTaskMemFree(p);
                }
                item->Release();
            }
        }
        dlg->Release();
    }
    if (SUCCEEDED(co)) CoUninitialize();
    ResetMenuInput(); // the key/button that opened the dialog was released while it had the focus
    return out;
#else
    (void)title;
    (void)start;
    return {};
#endif
}

std::string LogDir() {
    std::error_code ec;
    for (const fs::path& p : {ExeDir().parent_path() / "logs", ExeDir() / "logs", ExeDir().parent_path() / "work" / "logs"})
        if (fs::is_directory(p, ec)) return P8(p.lexically_normal());
    return P8((ExeDir().parent_path() / "logs").lexically_normal());
}

void RefreshSaveInfo() {
    g.save = saves::Inspect(NandRoot());
    const bool en = En();
    if (!g.save.found) {
        g.save_info = Tr("Nog geen save in de opslagmap (een nieuw spel).", "No save in the data folder yet (a new game).");
    } else {
        g.save_info = std::string(Tr("Save gevonden: ", "Save found: ")) + std::to_string(g.save.files) +
                      Tr(" bestanden", " files");
        if (!g.save.last_backup.empty())
            g.save_info += std::string(Tr("  \xC2\xB7  laatste reservekopie ", "  \xC2\xB7  last backup ")) + g.save.last_backup;
        g.save_info += std::string("\n") + Tr("Reservekopie\xC3\xABn: ", "Backups: ") + g.save.backup_root;
    }
    (void)en;
}

std::string SystemInfo() {
    std::string s = "SkyBreak " + std::string(kVersion) + " (build " + BuildDate() + ")\n";
#ifdef _WIN32
    char prod[128] = {}, ver[64] = {}, build[32] = {};
    DWORD n = sizeof prod;
    RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "ProductName", RRF_RT_REG_SZ, nullptr, prod, &n);
    n = sizeof ver;
    RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "DisplayVersion", RRF_RT_REG_SZ, nullptr, ver, &n);
    n = sizeof build;
    RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "CurrentBuild", RRF_RT_REG_SZ, nullptr, build, &n);
    s += std::string("Windows: ") + prod + " " + ver + " (build " + build + ")\n";
#endif
    s += "GPU: " + g.renderer + "\n";
    if (const char* v = reinterpret_cast<const char*>(glGetString(GL_VERSION))) s += std::string("OpenGL: ") + v + "\n";
    runtime::input::ControllerStatus st[4];
    runtime::input::controller_status(st);
    for (const auto& c : st)
        if (c.connected) s += std::string("Controller: ") + c.name + " (" + c.type + (c.has_gyro ? ", gyro" : "") + ")\n";
    s += "Disc check: " + g.disc.message + " " + g.disc.detail + "\n";
    s += std::string("Save: ") + (g.save.found ? "found" : "none") + ", backups " + std::to_string(g.save.backups) + "\n";
    s += "Settings file: " + g.settings_path + "\n";
    return s;
}

// Restart with the same command line (our own environment switches removed: they would look like the player's).
bool RestartSkyBreak() {
#ifdef _WIN32
    ClearBridgedEnv();
    std::wstring cl = GetCommandLineW();
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES; // the output keeps going to the same log (SkyBreak.exe start program)
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe, cl.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    std::cout << "[LAUNCH19] restarting SkyBreak (pid " << pi.dwProcessId << ")" << std::endl;
    SDL_Event q{};
    q.type = SDL_QUIT;
    SDL_PushEvent(&q);
    return true;
#else
    return false;
#endif
}

std::string PageAction(const std::string& id, std::string& title, bool& err) {
    const bool en = En();
    err = false;
    if (id == "act_import_dolphin" || id == "act_export_dolphin") {
        const bool imp = id == "act_import_dolphin";
        std::string start;
        if (const char* docs = std::getenv("USERPROFILE")) start = std::string(docs) + "\\Documents\\Dolphin Emulator";
        const std::string folder = PickFolder(imp ? Tr("Dolphin-map met je save", "Dolphin folder with your save")
                                                  : Tr("Dolphin-map voor je save", "Dolphin folder for your save"), start);
        if (folder.empty()) return {};
        const saves::Outcome o = imp ? saves::ImportFromDolphin(folder, NandRoot(), en) : saves::ExportToDolphin(NandRoot(), folder, en);
        title = imp ? Tr("Save importeren", "Import save") : Tr("Save exporteren", "Export save");
        err = !o.ok;
        RefreshSaveInfo();
        std::cout << "[LAUNCH19] " << (imp ? "import" : "export") << ": " << o.message << std::endl;
        return o.message;
    }
    if (id == "act_diag_zip") {
        diag::Request r;
        r.log_dir = g.log_dir;
        r.out_dir = P8(U8(g.log_dir).parent_path() / "diagnose");
        r.settings_path = g.settings_path;
        r.system_info = SystemInfo();
        std::string e;
        const std::string zip = diag::MakePackage(r, &e);
        title = Tr("Diagnosepakket", "Diagnostics package");
        if (zip.empty()) {
            err = true;
            return std::string(Tr("Maken mislukt: ", "Failed: ")) + e;
        }
        if (!g.selftest && !g.at) OpenFolder(P8(U8(zip).parent_path()));
        return std::string(Tr("Gemaakt: ", "Created: ")) + zip +
               Tr("\nDaarin: logbestanden, je instellingen en systeeminfo.txt. Niets van het spel of je save.",
                  "\nInside: log files, your settings and systeeminfo.txt. Nothing of the game or your save.");
    }
    if (id == "reset_all_backup") { // before "Alle instellingen terugzetten" writes the defaults
        std::error_code ec;
        const fs::path src = U8(g.settings_path);
        if (!fs::is_regular_file(src, ec)) return {};
        fs::path bak = src;
        for (int i = 1; i < 1000; ++i) {
            bak = src;
            bak += i == 1 ? std::string(".bak") : ".bak" + std::to_string(i);
            if (!fs::exists(bak, ec)) break;
        }
        fs::copy_file(src, bak, ec);
        return ec ? std::string() : std::string(Tr("Kopie: ", "Copy: ")) + P8(bak);
    }
    if (id == "restart_now") {
        if (!RestartSkyBreak()) {
            err = true;
            title = Tr("Opnieuw starten", "Restart");
            return Tr("SkyBreak kon zichzelf niet opnieuw starten. Sluit het en start het opnieuw.",
                      "SkyBreak could not restart itself. Close it and start it again.");
        }
        return {};
    }
    return {};
}

page::Pad LastPad() {
    const std::string d = ControlsQuery("last_device", g.edit, g.saved);
    if (d == "ps") return page::Pad::PlayStation;
    if (d == "xbox") return page::Pad::Xbox;
    if (d == "switch") return page::Pad::Switch;
    if (d == "kbm") return page::Pad::Keyboard;
    runtime::input::ControllerStatus st[4]; // nothing used yet: a connected pad, else the keyboard
    runtime::input::controller_status(st);
    for (const auto& c : st)
        if (c.connected) {
            const std::string t = c.type;
            return t.rfind("PS", 0) == 0 ? page::Pad::PlayStation
                   : (t.find("Switch") != std::string::npos || t.find("Joy") != std::string::npos) ? page::Pad::Switch
                                                                                                   : page::Pad::Xbox;
        }
    return page::Pad::Keyboard;
}

const char* EnvOfPlayer(const char* name) { return EnvSetByPlayer(name) ? std::getenv(name) : nullptr; }

page::Host MakePageHost(float s) {
    page::Host h;
    h.f.body = g.f_body;
    h.f.bold = g.f_bold;
    h.f.small = g.f_small;
    h.f.head = g.f_head;
    h.f.caps = g.f_caps;
    h.f.tab = g.f_tab;
    h.f.menu = g.f_menu;
    h.f.word = g.f_word;
    h.s = s;
    h.en = En();
    h.accent = AccentColour();
    h.in_game = g.in_game;
    h.selftest = g.selftest;
    h.now = Now();
    h.pad = LastPad();
    h.confirm_south = ConfirmSouth();
    h.edit = &g.edit;
    h.saved = &g.saved;
    h.boot = &g.boot;
    h.boot_app = &g.boot_app;
    h.ctx.in_game = g.in_game;
    h.ctx.disc_ok = g.disc_done.load() && g.disc.ready;
    h.ctx.hd_pack_found = true; // the loader decides at boot; the row only explains
    {
        runtime::input::ControllerStatus st[4];
        runtime::input::controller_status(st);
        for (const auto& c : st) h.ctx.gyro_pad = h.ctx.gyro_pad || (c.connected && c.has_gyro);
    }
    h.ctx.env = &EnvOfPlayer;
    h.game_dir = g.game_dir;
    h.nand_root = NandRoot();
    h.settings_path = g.settings_path;
    h.settings_problem = g.settings_problem;
    h.renderer = g.renderer;
    h.log_dir = g.log_dir;
    h.disc_done = g.disc_done.load();
    if (h.disc_done) {
        h.disc_ok = g.disc.ready;
        h.disc_message = g.disc.message;
        h.disc_detail = g.disc.detail;
        const bool en = h.en;
        h.disc_checks = {std::string("sys/boot.bin: ") + (g.disc.game_id.empty() ? "?" : g.disc.game_id) +
                             (en ? ", revision " : ", revisie ") + std::to_string(g.disc.revision),
                         std::string("sys/main.dol: ") + (g.disc.dol_ok ? (en ? "matches (SHA-1)" : "klopt (SHA-1)")
                                                                        : (en ? "differs" : "klopt niet")),
                         std::string("files/rels.arc: ") + (g.disc.rels_ok ? (en ? "matches (SHA-1)" : "klopt (SHA-1)")
                                                                           : (en ? "differs" : "klopt niet"))};
        h.disc_checks_ok = {g.disc.game_id == "SOUE01" && g.disc.revision == 0, g.disc.dol_ok, g.disc.rels_ok};
    }
    h.audio_devices = g.audio_devices;
    h.save_info = g.save_info;
    h.version_line = std::string("SkyBreak ") + kVersion + "  \xC2\xB7  build " + BuildDate();
    h.wing = g.wing_tex ? ImTextureID(intptr_t(g.wing_tex)) : ImTextureID(0);
    h.wing_sz = ImVec2(float(g.wing_w), float(g.wing_h));
    h.disc_logo = g.logo_tex ? ImTextureID(intptr_t(g.logo_tex)) : ImTextureID(0);
    h.disc_logo_sz = ImVec2(float(g.logo_w), float(g.logo_h));
    h.track = [](const std::string& l, ImVec2 a, ImVec2 b) { TrackRect(l, a, b); };
    h.preview = [](const Settings& e) { ApplyLive(e); };
    h.save = [](std::string& status) { return SaveSettings(status); };
    h.revert = [] { RevertEdits(); };
    h.open_folder = [](const std::string& p) {
        if (g.selftest || g.at) { std::cout << "[LAUNCH19] open folder " << p << std::endl; return; }
        std::error_code ec;
        const fs::path save = U8(p) / "title" / "00010000" / "534f5545";
        OpenFolder(fs::is_directory(save, ec) ? P8(save) : fs::is_regular_file(U8(p), ec) ? P8(U8(p).parent_path()) : p);
    };
    h.pick_folder = [](const std::string& t, const std::string& st) { return PickFolder(t, st); };
    h.action = [](const std::string& id, std::string& title, bool& err) { return PageAction(id, title, err); };
    h.controls.f_small = g.f_small;
    h.controls.f_bold = g.f_bold;
    h.controls.s = s;
    h.controls.in_game = g.in_game;
    h.controls.selftest = g.selftest;
    h.controls.track = [](const std::string& l) { Track(l); };
    h.controls.section = [](const char* t) { SectionTitle(t); };
    h.controls.hint = [](const char* t) { Hint(t); };
    return h;
}

// The settings page over the area a..b. Returns Back / Play (launcher: play; in game: resume) or None.
page::Result SettingsPage(ImVec2 a, ImVec2 b, float s) {
    page::Host h = MakePageHost(s);
    const page::Result r = page::Draw(g.pst, h, a, b);
    g.status = g.pst.status;
    return r;
}

void BeginPanel(const char* id, ImVec2 center, ImVec2 size) {
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::Begin(id, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                  ImGuiWindowFlags_NoCollapse);
}

// ---------------------------------------------------------------------------------------- launcher
void DrawBackdrop(ImDrawList* dl, ImVec2 a, ImVec2 b, float t) {
    ThpBackgroundFrame(g.game_dir, g.bg_tex, g.bg_w, g.bg_h); // THPVID19: video frame into the backdrop texture (ui_thp_player.h)
    const float W = b.x - a.x, H = b.y - a.y;
    if (g.bg_tex) {
        const float zoom = 1.03f + 0.025f * std::sin(t * 0.07f);
        const float sc = std::max(W / g.bg_w, H / g.bg_h) * zoom;
        const float w = g.bg_w * sc, h = g.bg_h * sc;
        const float x = a.x + (W - w) * 0.5f, y = a.y + (H - h) * 0.58f;
        dl->AddImage(ImTextureID(intptr_t(g.bg_tex)), ImVec2(x, y), ImVec2(x + w, y + h));
    } else {
        dl->AddRectFilledMultiColor(a, b, IM_COL32(92, 150, 210, 255), IM_COL32(92, 150, 210, 255),
                                    IM_COL32(236, 214, 170, 255), IM_COL32(236, 214, 170, 255));
    }
    // readability: dark wash on the left (menu) and the bottom (status), a light vignette on top
    dl->AddRectFilledMultiColor(a, ImVec2(a.x + W * 0.62f, b.y), IM_COL32(12, 9, 5, 215), IM_COL32(12, 9, 5, 0),
                                IM_COL32(12, 9, 5, 0), IM_COL32(12, 9, 5, 215));
    dl->AddRectFilledMultiColor(ImVec2(a.x, b.y - H * 0.28f), b, IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0),
                                IM_COL32(8, 6, 3, 190), IM_COL32(8, 6, 3, 190));
    dl->AddRectFilledMultiColor(a, ImVec2(b.x, a.y + H * 0.18f), IM_COL32(8, 6, 3, 120), IM_COL32(8, 6, 3, 120),
                                IM_COL32(8, 6, 3, 0), IM_COL32(8, 6, 3, 0));
}

void DrawLogo(ImDrawList* dl, ImVec2 p, float width) {
    if (g.logo_tex) {
        const float h = width * g.logo_h / std::max(1, g.logo_w);
        dl->AddImage(ImTextureID(intptr_t(g.logo_tex)), p, ImVec2(p.x + width, p.y + h));
        return;
    }
    ImFont* f = g.f_menu;
    dl->AddText(f, f->FontSize * 0.6f, p, kParchment, "THE LEGEND OF ZELDA");
    dl->AddText(f, f->FontSize * 1.6f, ImVec2(p.x, p.y + f->FontSize * 0.65f), kGold, "SKYWARD SWORD");
}

float LogoHeight(float width) {
    return g.logo_tex ? width * g.logo_h / std::max(1, g.logo_w) : g.f_menu->FontSize * 2.4f;
}

// LAUNCH19: the main screen = the THPVID19 film + SkyBreak identity + SPELEN / INSTELLINGEN / AFSLUITEN, the PRELAUNCH
// summary (disc, save, controller; each line opens its row in START), the game's own logo from the player's disc.
void MainSummary(ImDrawList* bg, float W, float H, float s) {
    const bool en = En();
    const ImU32 green = IM_COL32(120, 220, 120, 255), orange = IM_COL32(240, 190, 80, 255), red = IM_COL32(240, 90, 70, 255);
    struct Line { ImU32 dot; std::string text; const char* row; };
    std::vector<Line> lines;
    const bool done = g.disc_done.load();
    if (!done) lines.push_back({orange, Tr("Schijf controleren...", "Checking the disc..."), "game_dir"});
    else if (g.disc.ready)
        lines.push_back({green, Tr("Schijf gecontroleerd: The Legend of Zelda: Skyward Sword (USA 1.00)",
                                   "Disc checked: The Legend of Zelda: Skyward Sword (USA 1.00)"), "game_dir"});
    else lines.push_back({red, g.disc.message + (g.disc.detail.empty() ? "" : "  \xC2\xB7  " + g.disc.detail), "game_dir"});
    if (g.save.found) {
        std::string t = std::string(Tr("Save gevonden  \xC2\xB7  ", "Save found  \xC2\xB7  ")) + std::to_string(g.save.files) +
                        Tr(" bestanden", " files");
        if (!g.backup_msg.empty()) t += "  \xC2\xB7  " + g.backup_msg;
        else if (!g.save.last_backup.empty()) t += std::string(Tr("  \xC2\xB7  reservekopie ", "  \xC2\xB7  backup ")) + g.save.last_backup;
        lines.push_back({green, t, "save_backups"});
    } else {
        lines.push_back({orange, Tr("Nog geen save: je begint een nieuw spel", "No save yet: you start a new game"), "nand_root"});
    }
    {
        runtime::input::ControllerStatus st[4];
        runtime::input::controller_status(st);
        std::string pad;
        for (const auto& c : st)
            if (c.connected && pad.empty()) pad = c.name;
        const catalog::Desc* pd = catalog::Find(pad.empty() ? "preset_keyboard" : "preset_gamepad");
        std::string layout;
        if (pd) {
            const int i = catalog::ChoiceIndex(*pd, g.edit);
            if (i >= 0) layout = catalog::T(pd->choices[size_t(i)].label, en);
        }
        if (pad.empty())
            lines.push_back({green, std::string(Tr("Toetsenbord + muis  \xC2\xB7  indeling ", "Keyboard + mouse  \xC2\xB7  layout ")) + layout,
                             "preset_keyboard"});
        else
            lines.push_back({green, pad + Tr(" aangesloten  \xC2\xB7  indeling ", " connected  \xC2\xB7  layout ") + layout,
                             "preset_gamepad"});
    }
    float by = H - (36.0f + 30.0f * float(lines.size()) + 24.0f) * s;
    if (!g.settings_problem.empty()) { // CTRL18-UI's parse warning as a banner (GEAVANCEERD has the details)
        const std::string t = std::string(Tr("Je instellingenbestand heeft een fout (", "Your settings file has an error (")) +
                              g.settings_problem.substr(0, 90) + Tr("). Open INSTELLINGEN > GEAVANCEERD.", "). Open SETTINGS > ADVANCED.");
        const float w = std::min(W - 100 * s, g.f_small->CalcTextSizeA(g.f_small->FontSize, FLT_MAX, 0, t.c_str()).x + 28 * s);
        const ImVec2 a(52 * s, by - 44 * s), b(52 * s + w, by - 12 * s);
        bg->AddRectFilled(a, b, IM_COL32(60, 34, 14, 220), 6 * s);
        bg->AddRectFilled(a, ImVec2(a.x + 3 * s, b.y), IM_COL32(242, 168, 90, 255));
        bg->PushClipRect(a, b, true);
        bg->AddText(g.f_small, g.f_small->FontSize, ImVec2(a.x + 14 * s, a.y + 7 * s), IM_COL32(242, 168, 90, 255), t.c_str());
        bg->PopClipRect();
        TrackRect("banner", a, b);
        if (ImGui::IsMouseHoveringRect(a, b, false) && ImGui::IsMouseClicked(0)) {
            g.page = Page::Settings;
            g.edit = g.saved;
            g.pst.Open();
            g.pst.tab = int(catalog::Tab::Geavanceerd);
            g.pst.focus[g.pst.tab] = "settings_file";
        }
    }
    for (const Line& l : lines) {
        bg->AddCircleFilled(ImVec2(62 * s, by + g.f_body->FontSize * 0.55f), 5 * s, l.dot);
        bg->AddText(g.f_body, g.f_body->FontSize, ImVec2(76 * s, by), kParchment, l.text.c_str());
        const ImVec2 a(52 * s, by - 3 * s),
            b(76 * s + g.f_body->CalcTextSizeA(g.f_body->FontSize, FLT_MAX, 0, l.text.c_str()).x + 8 * s, by + g.f_body->FontSize + 4 * s);
        TrackRect(std::string("summary:") + l.row, a, b);
        if (ImGui::IsMouseHoveringRect(a, b, false)) {
            bg->AddLine(ImVec2(76 * s, b.y - 1 * s), ImVec2(b.x - 8 * s, b.y - 1 * s), IM_COL32(236, 200, 120, 140), 1 * s);
            if (ImGui::IsMouseClicked(0)) { // open START at that row
                g.page = Page::Settings;
                g.edit = g.saved;
                g.pst.Open();
                const catalog::Desc* d = catalog::Find(l.row);
                g.pst.tab = d ? int(d->tab) : 0;
                g.pst.focus[g.pst.tab] = l.row;
            }
        }
        by += 30 * s;
    }
}

void SaveFboPng(GLuint fbo, int w, int h, const std::string& path, std::vector<std::string>& written);

void TickDiscordPresence() {
    if (g.selftest) return;
    // Test runs never publish activity unless explicitly requested for an integration test.
    const bool test_run = std::getenv("NWII_RUN_SECONDS") || std::getenv("NWII_SS_INPUT_REPLAY") || std::getenv("NWII_HEADLESS");
    const char* test = std::getenv("NWII_DISCORD_TEST");
    if (test_run && !(test && test[0]=='1')) return;
    static double next = 0;
    if (Now() < next) return;
    next = Now()+1;
    const auto& u = runtime::ui_config();
    const char* force_off = std::getenv("NWII_DISCORD");
    runtime::discord::Update(u.discord_presence && !(force_off && force_off[0]=='0'),
        g.in_launcher, g.menu_open.load(), u.discord_show_location,
        runtime::progress::channel.get(), u.discord_large_image);
}

void TickAchievements() {
    if (!g.achievements_loaded || g.selftest) return;
    const auto notices = g.achievements.Observe(runtime::progress::channel.get(), int64_t(std::time(nullptr)));
    for (const size_t i : notices) {
        const auto& m = runtime::progress::milestones[i];
        PushToast(Tr("ACHIEVEMENT BEHAALD", "ACHIEVEMENT UNLOCKED"), En() ? m.en : m.nl);
    }
    if (g.achievements.dirty() && Now() >= g.achievement_retry) {
        if (!g.achievements.Save()) g.achievement_retry = Now() + 5;
    }
}

void AchievementPage(ImVec2 a, ImVec2 b, float s) {
    ImGui::SetNextWindowPos(ImVec2(a.x+40*s, a.y+32*s));
    ImGui::SetNextWindowSize(ImVec2(b.x-a.x-80*s, b.y-a.y-64*s));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.025f,0.065f,0.095f,0.96f));
    ImGui::Begin("##achievements_page", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PushFont(g.f_menu);
    ImGui::TextColored(ImVec4(1,0.885f,0.59f,1), "ACHIEVEMENTS");
    ImGui::PopFont();
    ImGui::PushFont(g.f_body);
    ImGui::Text("%d / %d  %s", g.achievements.count(), int(runtime::progress::milestones.size()), Tr("mijlpalen behaald", "milestones unlocked"));
    ImGui::ProgressBar(g.achievements.count()/float(runtime::progress::milestones.size()), ImVec2(-1,6*s), "");
    ImGui::Spacing();
    Hint(Tr("Jouw lokale collectie over alle saves. Bestaande voortgang wordt stil toegevoegd zodra je die save laadt.",
            "Your local collection across all saves. Existing progress is imported quietly when you load that save."));
    if (!g.achievements.error().empty()) ImGui::TextWrapped("%s: %s", Tr("Opslaan lukt niet", "Cannot save"), g.achievements.error().c_str());
    const float body_h = std::max(80*s, ImGui::GetContentRegionAvail().y-74*s);
    ImGui::BeginChild("##achievement_list", ImVec2(0,body_h));
    const bool scroll_down=ImGui::IsKeyDown(ImGuiKey_GamepadDpadDown)||ImGui::IsKeyDown(ImGuiKey_DownArrow);
    const bool scroll_up=ImGui::IsKeyDown(ImGuiKey_GamepadDpadUp)||ImGui::IsKeyDown(ImGuiKey_UpArrow);
    if (scroll_down||scroll_up) ImGui::SetScrollY(ImGui::GetScrollY()+(scroll_down ? 1 : -1)*450*s*ImGui::GetIO().DeltaTime);
    if (ImGui::BeginTable("##milestones", b.x-a.x > 900*s ? 2 : 1, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_PadOuterX)) {
        for (size_t i=0; i<runtime::progress::milestones.size(); ++i) {
            const auto& m = runtime::progress::milestones[i];
            const bool unlocked = (g.achievements.mask() & (1u << i)) != 0;
            ImGui::TableNextColumn();
            ImGui::PushID(int(i));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, unlocked ? ImVec4(.12f,.16f,.17f,1) : ImVec4(.055f,.105f,.14f,1));
            ImGui::BeginChild("##milestone", ImVec2(0,112*s), ImGuiChildFlags_Borders);
            ImGui::PushFont(g.f_bold);
            ImGui::TextColored(unlocked ? ImVec4(1,.885f,.59f,1) : ImVec4(.72f,.79f,.81f,1), "%s", En() ? m.en : m.nl);
            ImGui::PopFont();
            ImGui::TextWrapped("%s", En() ? m.desc_en : m.desc_nl);
            ImGui::PushFont(g.f_small);
            ImGui::TextColored(unlocked ? ImVec4(.59f,.88f,.75f,1) : ImVec4(.52f,.62f,.68f,1), "%s", unlocked ? Tr("BEHAALD", "UNLOCKED") : Tr("NOG TE ONTDEKKEN", "YET TO DISCOVER"));
            ImGui::PopFont();
            ImGui::EndChild(); ImGui::PopStyleColor(); ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild(); ImGui::PopFont();
    if (g.focus_menu) { ImGui::SetKeyboardFocusHere(); g.focus_menu=false; }
    if (MenuEntry("##achievement_back", Tr("TERUG", "BACK"), 0, s, 230*s) ||
        ImGui::IsKeyPressed(ImGuiKey_Escape,false) || ImGui::IsKeyPressed(BackKey(),false)) {
        g.page = Page::Main; g.focus_menu = true;
    }
    ImGui::End(); ImGui::PopStyleColor();
}

void ModsPage(ImVec2 a, ImVec2 b, float s) {
    namespace mods = runtime::mods;
    ImGui::SetNextWindowPos(ImVec2(a.x+40*s,a.y+32*s));
    ImGui::SetNextWindowSize(ImVec2(b.x-a.x-80*s,b.y-a.y-64*s));
    ImGui::PushStyleColor(ImGuiCol_WindowBg,ImVec4(.025f,.065f,.095f,.96f));
    ImGui::Begin("##mods_page",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings);
    SectionTitle("MODS");
    ImGui::PushFont(g.f_body);
    Hint(Tr("Modelswaps en textures. Plaats elke mod in een eigen map met mod.toml. Bovenaan wint bij conflicten.",
            "Model swaps and textures. Put each mod in its own folder with mod.toml. The top mod wins conflicts."));
    if(g.in_game) Hint(Tr("Aanpassingen gelden bij de volgende start van SkyBreak.","Changes apply the next time you start SkyBreak."));
    if(mods::RestartRequired()) ImGui::TextColored(ImVec4(1,.8f,.4f,1),"%s",Tr("Herstart SkyBreak om je wijzigingen te laden.","Restart SkyBreak to load your changes."));
    if(ImGui::Button(Tr("Modmap openen","Open mods folder"),ImVec2(190*s,36*s))) {
        std::error_code ec;fs::create_directories(mods::Root(),ec);
        if(!ec && !g.selftest && !g.at)OpenFolder(P8(mods::Root()));
    }
    Track(Tr("Modmap openen","Open mods folder"));ImGui::SameLine();
    if(ImGui::Button(Tr("Vernieuwen","Refresh"),ImVec2(140*s,36*s)))mods::Rescan();
    Track(Tr("Vernieuwen","Refresh"));
    if(!mods::Error().empty())ImGui::TextWrapped("%s",mods::Error().c_str());
    ImGui::BeginChild("##mods_list",ImVec2(0,std::max(65*s,ImGui::GetContentRegionAvail().y-62*s)));
    const auto list=mods::List();
    if(list.empty())ImGui::TextWrapped("%s",Tr("Nog geen mods. Open de modmap; LEESMIJ.md legt het formaat uit.","No mods yet. Open the mods folder; README.md explains the format."));
    for(size_t i=0;i<list.size();++i) {
        const auto& m=list[i];ImGui::PushID(int(i));
        ImGui::Separator();ImGui::PushFont(g.f_bold);
        ImGui::TextColored(ImVec4(1,.885f,.59f,1),"%s",m.name.c_str());ImGui::PopFont();
        ImGui::TextWrapped("%s  %s",m.version.c_str(),m.author.c_str());
        if(!m.description.empty())ImGui::TextWrapped("%s",m.description.c_str());
        ImGui::Text("%zu %s  /  %zu textures",m.files,Tr("gamebestanden","game files"),m.textures);
        if(m.conflicts)ImGui::TextColored(ImVec4(1,.8f,.4f,1),"%zu %s",m.conflicts,Tr("vervangingen verliezen van een mod hoger in de lijst","replacements lose to a mod above"));
        if(!m.error.empty())ImGui::TextWrapped("%s",m.error.c_str());
        ImGui::BeginDisabled(!m.error.empty()&&!m.enabled);bool enabled=m.enabled;
        if(ImGui::Checkbox(Tr("Ingeschakeld","Enabled"),&enabled))mods::Enable(m.id,enabled);
        Track("mod:"+m.id);ImGui::SameLine();
        ImGui::BeginDisabled(i==0);
        if(ImGui::Button(Tr("Omhoog","Up")))mods::Move(m.id,-1);
        Track("mod-up:"+m.id);ImGui::EndDisabled();ImGui::SameLine();
        ImGui::BeginDisabled(i+1==list.size());
        if(ImGui::Button(Tr("Omlaag","Down")))mods::Move(m.id,1);
        Track("mod-down:"+m.id);ImGui::EndDisabled();ImGui::EndDisabled();
        ImGui::Spacing();ImGui::PopID();
    }
    ImGui::EndChild();ImGui::PopFont();
    if(g.focus_menu){ImGui::SetKeyboardFocusHere();g.focus_menu=false;}
    if(MenuEntry("##mods_back",Tr("TERUG","BACK"),0,s,230*s) || ImGui::IsKeyPressed(ImGuiKey_Escape,false) || ImGui::IsKeyPressed(BackKey(),false)) {g.page=Page::Main;g.focus_menu=true;}
    ImGui::End();ImGui::PopStyleColor();
}

void ReportPage(ImVec2 a, ImVec2 b, float s) {
    ImGui::SetNextWindowPos(ImVec2(a.x+48*s,a.y+36*s));
    ImGui::SetNextWindowSize(ImVec2(b.x-a.x-96*s,b.y-a.y-72*s));
    ImGui::Begin("##problem_report",nullptr,ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
    SectionTitle(Tr("PROBLEEM VASTLEGGEN", "CAPTURE A PROBLEM"));
    Hint(Tr("Voor een plotselinge glitch: druk F8 tijdens het spelen. Hier kun je beschrijven wat er gebeurt en een nieuwe opname maken.",
            "For a sudden glitch: press F8 while playing. Here you can describe what happens and capture a new report."));
    ImGui::PushFont(g.f_body);
    ImGui::TextWrapped("%s", Tr("Wat zie je, wat deed je ervoor en hoe kom je bij deze plek?", "What do you see, what happened before it and how do you reach this spot?"));
    ImGui::InputTextMultiline("##report_note",g.report_note,sizeof(g.report_note),ImVec2(-1,130*s));
    Track("report_note");
    ImGui::Checkbox(Tr("Voeg mijn laatst opgeslagen Wii-save toe", "Include my last saved Wii save"),&g.report_save);
    Track("report_save");
    Hint(Tr("Een save is geen savestate: sla in het spel op vlak voor het probleem. De opname bevat beeld, locatie, instellingen en logs. Alles blijft op je pc.",
            "A save is not a savestate: save in the game just before the issue. A report contains an image, location, settings and logs. Everything stays on your PC."));
    if (ImGui::Button(Tr("Opname maken", "Capture report"),ImVec2(210*s,42*s))) { g.report_pending=true; g.report_error.clear(); }
    Track(Tr("Opname maken", "Capture report"));
    if (!g.report_path.empty()) {
        ImGui::SameLine();
        if (ImGui::Button(Tr("Map openen", "Open folder"),ImVec2(170*s,42*s))) OpenFolder(g.report_path);
        Track(Tr("Map openen", "Open folder"));
        ImGui::TextWrapped("%s: %s",Tr("Gemaakt", "Created"),g.report_path.c_str());
    }
    if (!g.report_error.empty()) ImGui::TextWrapped("%s: %s",Tr("Opname onvolledig", "Incomplete report"),g.report_error.c_str());
    ImGui::PopFont();
    if (MenuEntry("##report_back",Tr("TERUG", "BACK"),0,s,230*s)) { g.page=Page::Main; g.focus_menu=true; }
    ImGui::End();
}

void CaptureReport(GLuint fbo,int w,int h) {
    g.report_pending=false;
    g.report_error.clear();
    try {
        const auto p=runtime::progress::channel.get();
        fs::path dir=U8(g.log_dir).parent_path()/"reports";
        dir /= saves::Stamp()+"-"+std::to_string(SDL_GetTicks64());
        fs::create_directories(dir);
        g.report_path=P8(dir);
        std::vector<std::string> written;
        SaveFboPng(fbo,w,h,P8(dir/"screenshot.png"),written); // Presented game image before adding the port menu.
        if (written.empty()) throw std::runtime_error("Could not write the screenshot.");
        {
            std::ofstream f(dir/"context.txt",std::ios::binary);
            f << "SkyBreak " << kVersion << " launcher refresh / build " << BuildDate()
              << "\nSOUE01 / native PC runtime\nStage: " << p.stage << "\nRoom: " << p.room
              << "\nLayer: " << p.layer << "\nEntrance: " << p.entrance
              << "\nLink position: " << p.pos[0] << ", " << p.pos[1] << ", " << p.pos[2]
              << "\nPlaying: " << p.playing << "\nScreenshot: " << w << 'x' << h
              << "\n\nDescription:\n" << g.report_note
              << "\n\nImage is the last presented game frame; guest context may be one frame newer.\n"
              << "No savestate or GPU replay is included. Wii-save, if selected, is the last disk save.\n"
              << "For reproduction: supply route, repeatability, HD/interpolation on or off, and a short clip if helpful.\n";
            f.flush(); if (!f) throw std::runtime_error("Could not write the report context.");
        }
        diag::Request r; r.out_dir=g.report_path; r.log_dir=g.log_dir;
        r.settings_path=g.settings_path; r.system_info=SystemInfo();
        std::string err;
        if (diag::MakePackage(r,&err).empty()) throw std::runtime_error(err);
        if (g.report_save) {
            const fs::path save=U8(NandRoot())/"title/00010000/534f5545/data/wiiking2.sav";
            if (!fs::exists(save)) throw std::runtime_error("No saved Wii game was found.");
            fs::copy_file(save,dir/"wiiking2.sav");
        }
        PushToast(Tr("PROBLEEM VASTGELEGD", "PROBLEM CAPTURED"),Tr("Beeld, locatie en diagnose zijn bewaard.", "Image, location and diagnostics saved."));
        std::cout << "[LauncherRefresh] report: " << g.report_path << std::endl;
    } catch (const std::exception& e) {
        g.report_error=e.what();
        PushToast(Tr("OPNAME ONVOLLEDIG", "INCOMPLETE REPORT"),g.report_error);
    }
}

Action LauncherFrame(float W, float H, float s) {
    Action act = Action::None;
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    DrawBackdrop(bg, ImVec2(0, 0), ImVec2(W, H), float(Now()));
    const bool en = En();
    if (g.page != g.drawn_page) { g.drawn_page = g.page; g.page_since = Now(); }
    const float enter = float(std::clamp((Now() - g.page_since) / 0.28, 0.0, 1.0));
    if (g.page == Page::Main) {
        page::Host h = MakePageHost(s);
        const float wing = 118 * s;
        page::DrawBrand(bg, h, ImVec2(48 * s, 40 * s), wing, g.f_word ? g.f_word->FontSize : 64 * s, false);
        bg->AddText(g.f_small, g.f_small->FontSize, ImVec2(56*s, 40*s+wing+12*s), kMuted,
                    Tr("Skyward Sword op pc \xC2\xB7 jouw schijf, jouw avontuur", "Skyward Sword on PC \xC2\xB7 your disc, your adventure"));
        // Subtle motes at the edges; they never alter input rectangles.
        const float motion = float(Now());
        for (int i=0;i<12;++i) {
            const float x = W*(0.04f+0.072f*i) + std::sin(motion*.18f+i)*9*s;
            const float y = H-std::fmod(motion*(5+i%3)+i*57.0f,H);
            bg->AddCircleFilled(ImVec2(x,y),(1.2f+(i%2)*.6f)*s,IM_COL32(220,239,233,40));
        }
        const float menu_y = std::max(40 * s + wing + 56 * s, H * 0.37f) + (1-enter)*(1-enter)*12*s;
        bg->AddText(g.f_caps, g.f_caps->FontSize, ImVec2(56*s, menu_y-32*s), kMuted,
                    Tr("JOUW AVONTUUR BOVEN DE WOLKEN", "YOUR ADVENTURE ABOVE THE CLOUDS"));
        ImGui::SetNextWindowPos(ImVec2(44 * s, menu_y));
        ImGui::SetNextWindowSize(ImVec2(440 * s, 0));
        if (NavInputWithoutFocus()) g.focus_menu = true; // UIT14: a click beside the menu left keys/controller dead
        ImGui::Begin("##launcher", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                                                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                                ImGuiWindowFlags_AlwaysAutoResize);
        if (g.focus_menu) {
            ImGui::SetKeyboardFocusHere();
            g.focus_menu = false;
            g.show_nav = true;
        }
        if (MenuEntry("##play", en ? "PLAY" : "SPELEN", 0, s)) act = Action::Play;
        if (MenuEntry("##settings", en ? "SETTINGS" : "INSTELLINGEN", 1, s)) {
            g.page = Page::Settings;
            g.edit = g.saved;
            g.status.clear();
            g.pst.Open();
        }
        if (MenuEntry("##achievements", "ACHIEVEMENTS", 2, s)) { g.page = Page::Achievements; g.focus_menu = true; }
        if (MenuEntry("##mods", "MODS", 3, s)) { g.page=Page::Mods; g.focus_menu=true; }
        if (MenuEntry("##quit", en ? "QUIT" : "AFSLUITEN", 4, s)) act = Action::Quit;
        ImGui::Spacing();
        Hint(g.menu_sel == 0 ? Tr("Je schijf. Je save. Klaar voor de lucht.", "Your disc. Your save. Ready for the sky.") :
             g.menu_sel == 1 ? Tr("Maak beeld en besturing helemaal van jou.", "Make picture and controls your own.") :
             g.menu_sel == 2 ? Tr("Elke mijlpaal vertelt een verhaal.", "Every milestone tells a story.") :
             g.menu_sel == 3 ? Tr("Geef je avontuur een eigen uiterlijk.", "Give your adventure your own look.") :
                              Tr("Tot het volgende avontuur.", "Until the next adventure."));
        ImGui::End();
        // Start / Options / + and F5 = play from anywhere on the main screen (design B1)
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false) || ImGui::IsKeyPressed(ImGuiKey_F5, false)) act = Action::Play;
        MainSummary(bg, W, H, s);
        if (W > 1000*s) {
            const ImVec2 card(W-374*s, H*0.35f), end(W-40*s, H*0.35f+116*s);
            bg->AddRectFilled(card, end, IM_COL32(8, 25, 37, 205), 10*s);
            bg->AddRect(card, end, IM_COL32(183, 220, 228, 60), 10*s);
            bg->AddText(g.f_caps, g.f_caps->FontSize, ImVec2(card.x+20*s, card.y+17*s), kGold, "ACHIEVEMENTS");
            const std::string progress = std::to_string(g.achievements.count()) + " / 12  " + Tr("mijlpalen behaald", "milestones unlocked");
            bg->AddText(g.f_body, g.f_body->FontSize, ImVec2(card.x+20*s, card.y+48*s), kParchment, progress.c_str());
            bg->AddRectFilled(ImVec2(card.x+20*s, end.y-23*s), ImVec2(end.x-20*s, end.y-19*s), IM_COL32(130,155,170,65), 2*s);
            if (g.achievements.count()) bg->AddRectFilled(ImVec2(card.x+20*s, end.y-23*s),
                ImVec2(card.x+20*s+(end.x-card.x-40*s)*g.achievements.count()/12.0f, end.y-19*s), kGold, 2*s);
        }
        // the game being launched: its own title logo, read from the player's disc (not SkyBreak's identity)
        float vy = H - 30 * s;
        if (g.logo_tex) {
            const float lw = 250 * s, lh = lw * g.logo_h / std::max(1, g.logo_w);
            const ImVec2 lp(W - 40 * s - lw, H - 96 * s - lh);
            bg->AddImage(ImTextureID(intptr_t(g.logo_tex)), lp, ImVec2(lp.x + lw, lp.y + lh));
            const char* cap = Tr("van jouw schijf", "from your disc");
            const ImVec2 cs = g.f_caps->CalcTextSizeA(g.f_caps->FontSize, FLT_MAX, 0, cap);
            bg->AddText(g.f_caps, g.f_caps->FontSize, ImVec2(W - 42 * s - cs.x, lp.y + lh + 2 * s), kMuted, cap);
        }
        const std::string ver = std::string("SkyBreak ") + kVersion + "  \xC2\xB7  build " + BuildDate() + "  \xC2\xB7  " +
                                Tr("geen officieel Nintendo-product", "not an official Nintendo product");
        const ImVec2 vs = g.f_small->CalcTextSizeA(g.f_small->FontSize, FLT_MAX, 0, ver.c_str());
        bg->AddText(g.f_small, g.f_small->FontSize, ImVec2(W - 40 * s - vs.x, vy - vs.y * 0.5f), IM_COL32(190, 176, 146, 220), ver.c_str());
        // glyph hints (last used device; by position)
        float hx = W - 40 * s, hy = H - 70 * s;
        const float hh = 24 * s;
        const char* play_key = h.pad == page::Pad::Keyboard ? "F5" : h.pad == page::Pad::PlayStation ? "Options"
                               : h.pad == page::Pad::Switch ? "+" : "Menu";
        const float w2 = page::HintWidth(h, hh, -1, play_key, Tr("Spelen", "Play"));
        hx -= w2;
        page::DrawHint(bg, h, ImVec2(hx, hy), hh, -1, play_key, Tr("Spelen", "Play"));
        const float w1 = page::HintWidth(h, hh, page::ConfirmPos(h), "Enter", Tr("Kiezen", "Choose"));
        hx -= w1 + 22 * s;
        page::DrawHint(bg, h, ImVec2(hx, hy), hh, page::ConfirmPos(h), "Enter", Tr("Kiezen", "Choose"));
    } else if (g.page == Page::Achievements) {
        AchievementPage(ImVec2(0,0), ImVec2(W,H), s);
    } else if (g.page == Page::Mods) {
        ModsPage(ImVec2(0,0),ImVec2(W,H),s);
    } else {
        bg->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), IM_COL32(10, 8, 5, 90)); // LAUNCH19: the film a little darker behind the page
        const page::Result r = SettingsPage(ImVec2(0, 0), ImVec2(W, H), s);
        if (r == page::Result::Back) {
            g.page = Page::Main;
            g.focus_menu = true;
        } else if (r == page::Result::Play) {
            g.page = Page::Main;
            act = Action::Play;
        }
    }
    DrawToasts(ImGui::GetForegroundDrawList(), ImVec2(0, 0), ImVec2(W, H), s);
    return act;
}

// ---------------------------------------------------------------------------------------- in game
// Picture area of the window (points): the RES9 present rect published by the present code.
void WindowSize(int& ww, int& wh) {
    if (g.force_w > 0) { ww = g.force_w; wh = g.force_h; return; }
    SDL_GetWindowSize(g.window, &ww, &wh);
}

void PictureRect(ImVec2& a, ImVec2& b) {
    int ww = 0, wh = 0;
    WindowSize(ww, wh);
    const float x = runtime::input::g_present_rect[0].load(), y = runtime::input::g_present_rect[1].load();
    const float w = runtime::input::g_present_rect[2].load(), h = runtime::input::g_present_rect[3].load();
    a = ImVec2(x * ww, y * wh);
    b = ImVec2((x + w) * ww, (y + h) * wh);
    if (b.x - a.x < 32 || b.y - a.y < 32) { a = ImVec2(0, 0); b = ImVec2(float(ww), float(wh)); }
}

void SetMenuOpen(bool open) {
    if (open == g.menu_open.load()) return;
    g.menu_open.store(open);
    runtime::input::g_port_menu_open.store(open); // CTRL18-UI: kbm releases its mouse capture at once
    ControlsCancelCapture();                      // CTRL18-UI
    if (g.page != Page::Main && DiffEdits(g.saved, g.edit).size()) RevertEdits(); // LAUNCH19: live preview undone
    g.pst.Open();                                                               // LAUNCH19
    g.page = Page::Main;
    g.confirm_quit = false;
    g.edit = g.saved;
    g.status.clear();
    g.menu_sel = 0;
    g.focus_menu = true;
    if (open) {
        ResetMenuInput(); // UIT14: no key/button "still held" from the last time the menu was open (ui_widgets.h)
        SDL_SetRelativeMouseMode(SDL_FALSE);
        SDL_ShowCursor(SDL_ENABLE);
    }
    std::cout << "[UI13] in-game menu " << (open ? "open (game input suspended)" : "closed") << std::endl;
}

// UIT14: the cursor stays free while the menu is open. The keyboard/mouse source still runs then and turns
// relative mouse mode on when the aim key (Shift) goes down (hidden, locked cursor in the menu); it does not
// do that again while the key stays down, so switching it off once per press is enough.
void MenuMouseGuard() {
    if (!g.menu_open.load(std::memory_order_relaxed) || !SDL_GetRelativeMouseMode()) return;
    SDL_SetRelativeMouseMode(SDL_FALSE);
    SDL_ShowCursor(SDL_ENABLE);
    std::cout << "[UI13] menu open: relative mouse mode switched off again" << std::endl;
}

// LAUNCH19: [ui] perf_overlay = fps | frametime | full: a small counter top right of the picture. Presents are counted
// in ComposeFrame (every presented image passes there), game frames from the GX frame counter (g_stat_frames).
void PerfTick() {
    const std::string& mode = runtime::ui_config().perf_overlay;
    if (mode.empty() || mode == "off") return;
    const double now = Now();
    if (g.perf_t0 == 0) {
        g.perf_t0 = g.perf_last = now;
        g.perf_frames0 = g_stat_frames;
        return;
    }
    const float ms = float((now - g.perf_last) * 1000.0);
    g.perf_last = now;
    g.perf_ms_max = std::max(g.perf_ms_max, ms);
    g.perf_ms_acc += ms;
    ++g.perf_presents;
    if (now - g.perf_t0 >= 0.5) {
        const double dt = now - g.perf_t0;
        g.perf_fps = float(g.perf_presents / dt);
        g.perf_game_fps = float(double(g_stat_frames - g.perf_frames0) / dt);
        g.perf_ms = g.perf_presents ? g.perf_ms_acc / g.perf_presents : 0.0f;
        g.perf_t0 = now;
        g.perf_frames0 = g_stat_frames;
        g.perf_presents = 0;
        g.perf_ms_acc = 0;
        if (g.perf_ms_max > 0) g.perf_ms_max *= 0.5f; // decays: the worst frame of about the last second
    }
}
void DrawPerfOverlay(ImVec2 a, ImVec2 b, float s) {
    const std::string& mode = runtime::ui_config().perf_overlay;
    if (mode.empty() || mode == "off" || g.perf_fps <= 0) return;
    char l1[64], l2[96] = "", l3[96] = "";
    std::snprintf(l1, sizeof l1, "%.0f fps", g.perf_fps);
    if (mode == "frametime" || mode == "full") std::snprintf(l2, sizeof l2, "%.1f ms  (max %.1f)", g.perf_ms, g.perf_ms_max);
    if (mode == "full")
        std::snprintf(l3, sizeof l3, "%s %.0f fps  \xC2\xB7  %s %s", Tr("spel", "game"), g.perf_game_fps,
                      Tr("tussenbeelden", "in-between"), runtime::interp_config().enabled.load() ? Tr("aan", "on") : Tr("uit", "off"));
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const char* lines[3] = {l1, l2, l3};
    float w = 0, hgt = 0;
    for (const char* l : lines)
        if (*l) {
            w = std::max(w, g.f_small->CalcTextSizeA(g.f_small->FontSize, FLT_MAX, 0, l).x);
            hgt += g.f_small->FontSize + 2 * s;
        }
    const ImVec2 p0(b.x - w - 24 * s, a.y + 10 * s), p1(b.x - 10 * s, a.y + 18 * s + hgt);
    dl->AddRectFilled(p0, p1, IM_COL32(10, 8, 5, 170), 5 * s);
    float y = p0.y + 4 * s;
    for (const char* l : lines)
        if (*l) {
            dl->AddText(g.f_small, g.f_small->FontSize, ImVec2(p0.x + 7 * s, y), IM_COL32(236, 228, 210, 255), l);
            y += g.f_small->FontSize + 2 * s;
        }
}

void OverlayFrame(ImVec2 a, ImVec2 b, float s) {
    const bool open = g.menu_open.load();
    if (open) {
        ImGui::GetBackgroundDrawList()->AddRectFilled(a, b, IM_COL32(6, 4, 2, 150));
        const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
        if (g.page == Page::Main) {
            if (NavInputWithoutFocus()) g.focus_menu = true; // UIT14: a click beside the menu left keys/controller dead
            BeginPanel("##pause", c, ImVec2(470 * s, 0));
            ImGui::PushFont(g.f_menu);
            ImGui::TextColored(ImVec4(0.925f, 0.784f, 0.470f, 1.0f), "SKYBREAK"); // LAUNCH19 (was MENU)
            ImGui::PopFont();
            ImGui::Separator();
            if (g.focus_menu) {
                ImGui::SetKeyboardFocusHere();
                g.focus_menu = false;
                g.show_nav = true;
            }
            const float menu_w = ImGui::GetContentRegionAvail().x;
            if (MenuEntry("##resume", Tr("VERDER SPELEN", "RESUME"), 0, s, menu_w)) SetMenuOpen(false);
            if (g.menu_open.load() && MenuEntry("##settings", Tr("INSTELLINGEN", "SETTINGS"), 1, s, menu_w)) {
                g.page = Page::Settings;
                g.edit = g.saved;
                g.pst.Open(); // LAUNCH19
            }
            if (g.menu_open.load() && MenuEntry("##achievements", "ACHIEVEMENTS", 2, s, menu_w)) { g.page = Page::Achievements; g.focus_menu=true; }
            if (g.menu_open.load() && MenuEntry("##report", Tr("PROBLEEM VASTLEGGEN", "CAPTURE A PROBLEM"), 3, s, menu_w)) g.page = Page::Report;
            if (g.menu_open.load() && MenuEntry("##mods", "MODS", 4, s, menu_w)) {g.page=Page::Mods;g.focus_menu=true;}
            if (g.menu_open.load() && MenuEntry("##quit", Tr("AFSLUITEN", "QUIT"), 5, s, menu_w)) g.confirm_quit = true;
            ImGui::Spacing();
            Hint(Tr("Het spel loopt door; je invoer gaat naar dit menu. F1 of Terug: verder spelen.",
                    "The game keeps running; your input goes to this menu. F1 or Back: resume."));
            if (g.confirm_quit) {
                ImGui::Separator();
                ImGui::PushFont(g.f_bold);
                ImGui::TextWrapped("%s", Tr("Afsluiten? Voortgang die je niet in het spel hebt opgeslagen gaat verloren.", // REVLAUNCH19: NL/EN
                                            "Quit? Progress you did not save in the game is lost."));
                ImGui::PopFont();
                const bool yes = ImGui::Button(Tr("Ja, afsluiten", "Yes, quit"), ImVec2(180 * s, 0));
                Track(Tr("Ja, afsluiten", "Yes, quit"));
                if (yes) {
                    SDL_Event q{};
                    q.type = SDL_QUIT;
                    SDL_PushEvent(&q);
                    SetMenuOpen(false);
                }
                ImGui::SameLine();
                if (ImGui::Button(Tr("Nee", "No"), ImVec2(120 * s, 0))) g.confirm_quit = false;
                Track(Tr("Nee", "No"));
            }
            if (ImGui::IsKeyPressed(BackKey(), false) && !ImGui::IsAnyItemActive()) { // LAUNCH19: back BY POSITION
                if (g.confirm_quit) g.confirm_quit = false;
                else SetMenuOpen(false);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false)) SetMenuOpen(false); // LAUNCH19: Start = resume
            ImGui::End();
        } else if (g.page == Page::Achievements) {
            AchievementPage(a,b,s);
        } else if (g.page == Page::Report) {
            ReportPage(a,b,s);
        } else if (g.page == Page::Mods) {
            ModsPage(a,b,s);
        } else {
            const page::Result r = SettingsPage(a, b, s);
            if (r == page::Result::Back) {
                g.page = Page::Main;
                g.focus_menu = true;
            } else if (r == page::Result::Play) {
                SetMenuOpen(false);
            }
        }
    }
    DrawPerfOverlay(a, b, s); // LAUNCH19: [ui] perf_overlay
    DrawToasts(ImGui::GetForegroundDrawList(), a, b, s);
}

void EnsureUiFbo(int w, int h) {
    if (g.ui_fbo && g.ui_w == w && g.ui_h == h) return;
    // Runs inside the game's frame loop: keep the renderer's texture / unpack-buffer bindings.
    GLint tex = 0, unpack = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack);
    if (unpack) glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    if (!g.ui_tex) glGenTextures(1, &g.ui_tex);
    glBindTexture(GL_TEXTURE_2D, g.ui_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, GLuint(tex));
    if (unpack) glBindBuffer(GL_PIXEL_UNPACK_BUFFER, GLuint(unpack));
    if (!g.ui_fbo) glGenFramebuffers(1, &g.ui_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g.ui_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g.ui_tex, 0);
    g.ui_w = w;
    g.ui_h = h;
}

// Draws the current ImGui frame into `fbo` (fw x fh) as if the window rectangle a..b were the
// whole framebuffer. GL state the renderer may rely on is restored.
void RenderInto(GLuint fbo, int fw, int fh, ImVec2 a, ImVec2 b) {
    ImDrawData* dd = ImGui::GetDrawData();
    if (!dd) return;
    dd->DisplayPos = a;
    dd->DisplaySize = ImVec2(b.x - a.x, b.y - a.y);
    dd->FramebufferScale = ImVec2(fw / dd->DisplaySize.x, fh / dd->DisplaySize.y);
    GLint draw_fbo = 0, read_fbo = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
    GLboolean mask[4];
    glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    const GLboolean logic = glIsEnabled(GL_COLOR_LOGIC_OP), srgb = glIsEnabled(GL_FRAMEBUFFER_SRGB);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    if (logic) glDisable(GL_COLOR_LOGIC_OP);
    if (srgb) glDisable(GL_FRAMEBUFFER_SRGB);
    ImGui_ImplOpenGL3_RenderDrawData(dd);
    if (logic) glEnable(GL_COLOR_LOGIC_OP);
    if (srgb) glEnable(GL_FRAMEBUFFER_SRGB);
    glColorMask(mask[0], mask[1], mask[2], mask[3]);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(draw_fbo));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(read_fbo));
}

bool InitImGui(SDL_Window* window, SDL_GLContext ctx) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad |
                      ImGuiConfigFlags_NoMouseCursorChange;
    if (!ImGui_ImplSDL2_InitForOpenGL(window, ctx) || !ImGui_ImplOpenGL3_Init("#version 330")) {
        std::cout << "[UI13] ImGui backends failed; UI off" << std::endl;
        ImGui::DestroyContext();
        return false;
    }
    // Never let the backend open/poll controllers itself (INPUT13: only the controller thread may call
    // SDL joystick functions); FeedGamepad turns the posted controller events into ImGui input.
    ImGui_ImplSDL2_SetGamepadMode(ImGui_ImplSDL2_GamepadMode_Manual, nullptr, 0);
    ImGui_ImplOpenGL3_CreateDeviceObjects(); // now, so EnsureFonts can swap the font texture
    g.imgui = true;
    g.window = window;
#ifdef _WIN32
    if (void* lib = SDL_LoadObject("dwmapi.dll"))
        g.dwm_flush = (long(__stdcall*)())SDL_LoadFunction(lib, "DwmFlush");
#endif
    return true;
}

void NewUiFrame(float W, float H) {
    EnsureFonts(UiScale(W, H));
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    if (g.selftest) ImGui::GetIO().DeltaTime = 1.0f/60.0f; // deterministic offscreen motion snapshots
    if (g.force_w > 0) {
        ImGui::GetIO().DisplaySize = ImVec2(float(g.force_w), float(g.force_h));
        ImGui::GetIO().DisplayFramebufferScale = ImVec2(1, 1);
    }
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_HasGamepad; // fed by FeedGamepad
    ImGui::GetIO().ConfigNavSwapGamepadButtons = !ConfirmSouth(); // LAUNCH19: east confirms (Switch layout), south = back
    // UIT14 autotest: the backend adds the OS cursor position every frame while the window has focus; the
    // script's virtual cursor stands in for it (after the backend, so it wins).
    if (g.at && g.at_cursor) ImGui::GetIO().AddMousePosEvent(g.at_x, g.at_y);
    g.rects_next.clear();
    ImGui::NewFrame();
}

// After ImGui::Render(): the widget rectangles of this frame become the autotest's click targets.
void EndUiFrame() {
    if (g.at) g.rects.swap(g.rects_next);
}

void StartDiscCheck() {
    g.disc_thread = std::thread([] {
        g.disc = CheckDisc(g.game_dir);
        g.disc_done.store(true);
        std::cout << "[UI13] disc check: " << g.disc.message << " " << g.disc.detail << std::endl;
    });
}

void TrackWindowSize() {
    int ww = 0, wh = 0;
    SDL_GetWindowSize(g.window, &ww, &wh);
    if (ww > 0 && wh > 0) {
        Config::get().window_width = ww;
        Config::get().window_height = wh;
    }
}

// ---------------------------------------------------------------------------------------- autotest (UIT14)
// The script's input goes through SDL's own senders (keyboard state, modifiers, mouse state and focus as for a
// player), controller buttons as the SDL events INPUT13's controller thread posts. Main thread only.
SDL_Scancode KeyScancode(const std::string& name) {
    std::string n;
    for (char c : name) n += char(std::tolower(static_cast<unsigned char>(c)));
    static const std::pair<const char*, SDL_Scancode> kAlias[] = {
        {"esc", SDL_SCANCODE_ESCAPE},   {"escape", SDL_SCANCODE_ESCAPE}, {"enter", SDL_SCANCODE_RETURN},
        {"return", SDL_SCANCODE_RETURN}, {"kpenter", SDL_SCANCODE_KP_ENTER}, {"alt", SDL_SCANCODE_LALT},
        {"ralt", SDL_SCANCODE_RALT},    {"shift", SDL_SCANCODE_LSHIFT},  {"rshift", SDL_SCANCODE_RSHIFT},
        {"ctrl", SDL_SCANCODE_LCTRL},   {"space", SDL_SCANCODE_SPACE},   {"tab", SDL_SCANCODE_TAB},
        {"up", SDL_SCANCODE_UP},        {"down", SDL_SCANCODE_DOWN},     {"left", SDL_SCANCODE_LEFT},
        {"right", SDL_SCANCODE_RIGHT},  {"backspace", SDL_SCANCODE_BACKSPACE}, {"home", SDL_SCANCODE_HOME}};
    for (const auto& [a, sc] : kAlias)
        if (n == a) return sc;
    if (n.size() >= 2 && n[0] == 'f' && std::isdigit(static_cast<unsigned char>(n[1]))) {
        const int k = std::atoi(n.c_str() + 1);
        if (k >= 1 && k <= 12) return SDL_Scancode(SDL_SCANCODE_F1 + k - 1);
    }
    if (n.size() == 1 && n[0] >= 'a' && n[0] <= 'z') return SDL_Scancode(SDL_SCANCODE_A + (n[0] - 'a'));
    if (n.size() == 1 && n[0] >= '1' && n[0] <= '9') return SDL_Scancode(SDL_SCANCODE_1 + (n[0] - '1'));
    if (n == "0") return SDL_SCANCODE_0;
    return SDL_GetScancodeFromName(name.c_str());
}

int PadButton(const std::string& n) {
    static const std::pair<const char*, int> kPad[] = {
        {"a", SDL_CONTROLLER_BUTTON_A},          {"b", SDL_CONTROLLER_BUTTON_B},
        {"x", SDL_CONTROLLER_BUTTON_X},          {"y", SDL_CONTROLLER_BUTTON_Y},
        {"back", SDL_CONTROLLER_BUTTON_BACK},    {"guide", SDL_CONTROLLER_BUTTON_GUIDE},
        {"start", SDL_CONTROLLER_BUTTON_START},  {"lb", SDL_CONTROLLER_BUTTON_LEFTSHOULDER},
        {"rb", SDL_CONTROLLER_BUTTON_RIGHTSHOULDER}, {"up", SDL_CONTROLLER_BUTTON_DPAD_UP},
        {"down", SDL_CONTROLLER_BUTTON_DPAD_DOWN}, {"left", SDL_CONTROLLER_BUTTON_DPAD_LEFT},
        {"right", SDL_CONTROLLER_BUTTON_DPAD_RIGHT}, {"misc1", SDL_CONTROLLER_BUTTON_MISC1}};
    for (const auto& [a, b] : kPad)
        if (n == a) return b;
    return -1;
}

std::string SettingValue(const Settings& s, const std::string& k) {
    // LAUNCH19: every catalog value row by its id ("1"/"0", numbers, strings; e.g. edit.aspect, saved.master_volume)
    if (const catalog::Desc* d = catalog::Find(k); d && catalog::IsValueRow(*d)) return page::RawValue(*d, s);
    if (k == "background") return s.background;
    return ControlsQuery(std::string(&s == &g.edit ? "editvalue:" : "value:") + k, g.edit, g.saved); // CTRL18-UI
}

// CTRL18-UI: an autotest must not depend on where the player's real mouse cursor rests: move it out of the window
// (the virtual cursor of the script stands in for it, see NewUiFrame). Only in autotest runs.
void ParkRealCursor() {
    if (!g.window) return;
    int wx = 0, wy = 0, ww = 0, wh = 0;
    SDL_GetWindowPosition(g.window, &wx, &wy);
    SDL_GetWindowSize(g.window, &ww, &wh);
    SDL_Rect d{};
    SDL_GetDisplayBounds(std::max(0, SDL_GetWindowDisplayIndex(g.window)), &d);
    int px = wx + ww + 40, py = wy + wh / 2; // right of the window, else left, else below
    if (px >= d.x + d.w) px = wx - 40;
    if (px < d.x) {
        px = wx + ww / 2;
        py = wy + wh + 40 < d.y + d.h ? wy + wh + 40 : d.y + 2;
    }
    SDL_WarpMouseGlobal(px, py);
    std::cout << "[UIT14] real cursor parked at " << px << "," << py << " (outside the window)" << std::endl;
}

struct AppHost final : autotest::Host {
    double Now() override { return double(SDL_GetPerformanceCounter()) / double(SDL_GetPerformanceFrequency()); }
    bool Key(const std::string& name, bool down) override {
        const SDL_Scancode sc = KeyScancode(name);
        if (sc == SDL_SCANCODE_UNKNOWN) return false;
        SDL_SendKeyboardKey(down ? SDL_PRESSED : SDL_RELEASED, sc);
        return true;
    }
    void MouseMove(float nx, float ny) override {
        int ww = 0, wh = 0;
        WindowSize(ww, wh);
        g.at_x = nx * ww;
        g.at_y = ny * wh;
        g.at_cursor = true;
        SDL_SendMouseMotion(g.window, 0, 0, int(std::lround(g.at_x)), int(std::lround(g.at_y)));
    }
    void MouseButton(int b, bool down) override {
        SDL_SendMouseButton(g.window, 0, down ? SDL_PRESSED : SDL_RELEASED, Uint8(b));
    }
    bool Pad(const std::string& name, bool down) override {
        const int b = PadButton(name);
        if (b < 0) return false;
        SDL_Event e{};
        e.type = down ? SDL_CONTROLLERBUTTONDOWN : SDL_CONTROLLERBUTTONUP;
        e.cbutton.timestamp = SDL_GetTicks();
        e.cbutton.which = -1; // LAUNCH19: no real pad: FeedGamepad uses the generic (Xbox) positions, whatever is connected
        e.cbutton.button = Uint8(b);
        e.cbutton.state = down ? SDL_PRESSED : SDL_RELEASED;
        SDL_PushEvent(&e);
        return true;
    }
    bool WidgetCenter(const std::string& label, float& nx, float& ny) override {
        const auto it = g.rects.find(label);
        int ww = 0, wh = 0;
        WindowSize(ww, wh);
        if (it == g.rects.end() || ww <= 0 || wh <= 0) return false;
        nx = (it->second.first.x + it->second.second.x) * 0.5f / ww;
        ny = (it->second.first.y + it->second.second.y) * 0.5f / wh;
        return true;
    }
    std::string Query(const std::string& q) override {
        const auto b01 = [](bool v) { return std::string(v ? "1" : "0"); };
        const auto f = [](double v) { char t[32]; std::snprintf(t, sizeof t, "%.4f", v); return std::string(t); };
        int ww = 0, wh = 0, dw = 0, dh = 0;
        if (g.window) {
            SDL_GetWindowSize(g.window, &ww, &wh);
            SDL_GL_GetDrawableSize(g.window, &dw, &dh);
        }
        if (q == "launcher") return b01(g.in_launcher);
        if (q == "in_game") return b01(g.in_game);
        if (q == "menu_open") return b01(g.menu_open.load());
        if (q == "page") return g.page == Page::Main ? "main" : g.page == Page::Achievements ? "achievements" : g.page == Page::Report ? "report" : g.page == Page::Mods ? "mods" : "settings";
        if (q == "mod_count") return std::to_string(runtime::mods::List().size());
        if (q == "mod_restart") return runtime::mods::RestartRequired() ? "1" : "0";
        if (q.rfind("mod_enabled:",0)==0) {for(const auto& m:runtime::mods::List())if(m.id==q.substr(12))return m.enabled?"1":"0";return "missing";}
        if (q == "achievements") return std::to_string(g.achievements.count());
        if (q == "progress_stage") return runtime::progress::channel.get().stage;
        if (q == "progress_playing") return runtime::progress::channel.get().playing ? "1" : "0";
        if (q == "report_path") return g.report_path;
        if (q == "report_error") return g.report_error;
        if (q == "confirm_quit") return b01(g.confirm_quit);
        if (q == "fullscreen") return b01(g.window && (SDL_GetWindowFlags(g.window) & SDL_WINDOW_FULLSCREEN));
        if (q == "window_w") return std::to_string(ww);
        if (q == "window_h") return std::to_string(wh);
        if (q == "drawable_w") return std::to_string(dw);
        if (q == "drawable_h") return std::to_string(dh);
        if (q == "cfg_window_w") return std::to_string(Config::get().window_width);
        if (q == "cfg_window_h") return std::to_string(Config::get().window_height);
        if (q.rfind("rect_", 0) == 0 && q.size() == 6 && q[5] >= '0' && q[5] <= '3')
            return f(runtime::input::g_present_rect[q[5] - '0'].load());
        if (q == "relative_mouse") return b01(SDL_GetRelativeMouseMode());
        if (q == "cursor_shown") return b01(SDL_ShowCursor(SDL_QUERY) == SDL_ENABLE);
        if (q == "keyboard_focus") return b01(g.window && SDL_GetKeyboardFocus() == g.window);
        if (q == "wii_buttons") return std::to_string(runtime::input::InputManager::get().get_wiimote_state(0).buttons);
        if (q == "message_known") return b01(runtime::input::g_guest_context.get().message_known);
        if (q == "message_modal") return b01(runtime::input::g_guest_context.get().message_modal);
        if (q == "ir_x") return f(runtime::input::InputManager::get().get_wiimote_state(0).ir_x);
        if (q == "ir_y") return f(runtime::input::InputManager::get().get_wiimote_state(0).ir_y);
        if (q == "mpls_pointer") return b01(runtime::input::g_guest_motionplus_pointer[0].load());
        if (q == "gptr_x") return f(runtime::input::g_guest_pointer_x.load()); // the game's own pointer (-1..1)
        if (q == "gptr_y") return f(runtime::input::g_guest_pointer_y.load());
        if (q == "gptr_reads") return std::to_string(runtime::input::g_guest_pointer_reads.load());
        if (q == "aim_owner") return std::to_string(runtime::input::g_aim_owner.load());
        // "guest32:ADDR" = the guest u32 at ADDR, "guest32:ADDR+OFF" = the u32 at (u32 at ADDR) + OFF; main RAM only
        // (MEM1/MEM2, no MMIO), read only. E.g. guest32:0x805751E8+0xC = dPadManager_c::mMode (0 Normal,
        // 4 Calibrating, 6 PointCenter).
        if (q.rfind("guest32:", 0) == 0) {
            runtime::MMU* m = runtime::g_mmu;
            const auto ram = [](uint32_t a) {
                return (a >= 0x80000000u && a <= 0x817FFFFCu) || (a >= 0x90000000u && a <= 0x93FFFFFCu);
            };
            const std::string spec = q.substr(8);
            const size_t plus = spec.find('+');
            uint32_t a = uint32_t(std::strtoul(spec.substr(0, plus).c_str(), nullptr, 0));
            if (!m || !ram(a)) return "none";
            if (plus != std::string::npos) {
                a = m->read32(a) + uint32_t(std::strtoul(spec.substr(plus + 1).c_str(), nullptr, 0));
                if (!ram(a)) return "none";
            }
            return std::to_string(m->read32(a));
        }
        // Pointer diagnostics (read only; the producer state is main-thread owned in a window run, like this query)
        if (q == "point_center") return b01(runtime::input::g_guest_point_center.load());
        if (q == "pointer_cursor") return b01(runtime::input::g_guest_pointer_cursor.load());
        if (q == "aim_off_x") return f(runtime::input::g_pointer_aim[0].off_x);
        if (q == "aim_off_y") return f(runtime::input::g_pointer_aim[0].off_y);
        if (q == "aim_learned") return std::to_string(runtime::input::g_pointer_aim[0].learned);
        if (q == "vptr_x" || q == "vptr_y") { // the pointer of our virtual orientation (identity-centre model)
            float px = 9.0f, py = 9.0f;
            runtime::input::PointerAimLoop::pointer_of(runtime::input::g_virtual_orientation[0], px, py);
            return f(q == "vptr_x" ? px : py);
        }
        if (q == "toasts") return std::to_string(g.toasts.size());
        if (q == "status") return g.status;
        if (q == "frames") return std::to_string(g_stat_frames);
        if (q == "settings_path") return g.settings_path;
        if (q == "cfg_aspect") return std::to_string(Config::get().aspect_mode);
        if (q == "cfg_fullscreen") return b01(Config::get().fullscreen);
        if (q == "cfg_internal_scale") return std::to_string(Config::get().internal_scale);
        if (q == "ui_key_a_held") return b01(ImGui::IsKeyDown(ImGuiKey_A));
        if (q.rfind("saved.", 0) == 0) return SettingValue(g.saved, q.substr(6));
        if (q.rfind("edit.", 0) == 0) return SettingValue(g.edit, q.substr(5));
        if (q.rfind("widget:", 0) == 0) return b01(g.rects.count(q.substr(7)) > 0);
        if (q == "nav" || q == "nav_visible" || q == "nav_window" || q == "active_id") {
            ImGuiContext* c = ImGui::GetCurrentContext();
            if (!c) return "none";
            if (q == "nav_visible") return b01(c->NavCursorVisible);
            if (q == "nav_window") return c->NavWindow ? c->NavWindow->Name : "none";
            if (q == "active_id") return std::to_string(c->ActiveId);
            for (const auto& [label, id] : g.ids)
                if (id == c->NavId && c->NavId) return label;
            return c->NavId ? "id" + std::to_string(c->NavId) : "none";
        }
        if (q == "settings_problem") return g.settings_problem.empty() ? "none" : g.settings_problem; // CTRL18-UI
        // LAUNCH19: the settings page (tab, focus, dirty, restart_count, dialog, sub, avail.<id>, row.<id>, restart.<id>)
        if (q == "tab" || q == "tab_name" || q == "focus" || q == "dirty" || q == "restart_count" || q == "dialog" || q == "sub" ||
            q == "dialog_text" || q == "page_status" || q == "dialog_binds" /* REVLAUNCH19 */ ||
            q.rfind("avail.", 0) == 0 || q.rfind("row.", 0) == 0 ||
            q.rfind("restart.", 0) == 0) {
            page::Host h = MakePageHost(g.font_scale > 0 ? g.font_scale : 1.0f);
            return page::Query(g.pst, h, q);
        }
        if (q == "ui_scale") return f(g.font_scale);
        if (q == "output_gain") return f(runtime::audio::output_gain());
        if (q == "backup_msg") return g.backup_msg.empty() ? "none" : g.backup_msg;
        if (q == "settings_bak") { std::error_code ec; return b01(fs::exists(U8(g.settings_path + ".bak"), ec)); }
        if (q == "save_found") return b01(g.save.found);
        if (q == "window_title") return g.window ? SDL_GetWindowTitle(g.window) : "";
        if (q == "nav_swap") return b01(ImGui::GetCurrentContext() && ImGui::GetIO().ConfigNavSwapGamepadButtons);
        if (q == "perf_fps") return f(g.perf_fps);
        if (q.rfind("env:", 0) == 0) { const char* v = std::getenv(q.substr(4).c_str()); return v ? v : "unset"; }
        if (const std::string c = ControlsQuery(q, g.edit, g.saved); c != "?") return c;          // CTRL18-UI
        if (q == "last_key") return std::to_string(g.dbg_key);
        if (q == "last_key_window_ok") return b01(g.window && g.dbg_key_win == SDL_GetWindowID(g.window));
        return "?";
    }
    std::string Do(const std::vector<std::string>& a) override {
        const std::string& op = a[0];
        if (op == "focus") {
            if (!g.window) return "no window";
            SDL_SetKeyboardFocus(g.window); // what SDL does on WM_SETFOCUS (no OS focus is taken)
            ParkRealCursor();               // CTRL18-UI: the real cursor no longer decides what is hovered
            MouseMove(0.5f, 0.5f);
            return "";
        }
        if (op == "park") { // CTRL18-UI
            ParkRealCursor();
            return "";
        }
        if (op == "scroll_to" && a.size() == 2 && g.page == Page::Settings && !g.pst.sub_controls) {
            // LAUNCH19: on the settings page = focus the row with that label (NL or EN) in the current tab
            for (const catalog::Desc& d : catalog::All())
                if ((a[1] == d.label.nl || a[1] == d.label.en) && (int(d.tab) == g.pst.tab || int(d.also) == g.pst.tab)) {
                    g.pst.focus[g.pst.tab] = d.id;
                    g.pst.scroll_to_focus = true;
                    return "";
                }
            return "row not on this tab: " + a[1];
        }
        if (op == "scroll_to" && a.size() == 2) { // CTRL18-UI: scroll the page so that widget <label> is centred
            g.at_scroll_to = a[1];
            return "";
        }
        if (op == "refocus") { // CTRL18-UI: keyboard focus back to the window without moving the (virtual) mouse
            if (!g.window) return "no window";
            SDL_SetKeyboardFocus(g.window);
            return "";
        }
        if (op == "wheel" && a.size() == 2) { // CTRL18-UI: mouse wheel at the virtual cursor (scroll a long page)
            SDL_Event w{};
            w.type = SDL_MOUSEWHEEL;
            w.wheel.windowID = g.window ? SDL_GetWindowID(g.window) : 0;
            w.wheel.y = std::atoi(a[1].c_str());
            w.wheel.preciseY = float(w.wheel.y);
            w.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
            SDL_PushEvent(&w);
            return "";
        }
        if (op == "resize" && a.size() == 3) {
            SDL_SetWindowSize(g.window, std::atoi(a[1].c_str()), std::atoi(a[2].c_str()));
            return "";
        }
        if (op == "fullscreen" && a.size() == 2) {
            SDL_SetWindowFullscreen(g.window, a[1] == "1" ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
            return "";
        }
        if (op == "auto_point_center" && a.size() == 2) {
            g.at_auto_pc = a[1] == "1";
            return "";
        }
        if (op == "toast" && a.size() >= 2) {
            using K = runtime::input::ControllerEventKind;
            const std::string& k = a[1];
            const K kind = k == "connected" ? K::Connected : k == "disconnected" ? K::Disconnected
                         : k == "blocked" ? K::DetectionBlocked : k == "recovered" ? K::DetectionRecovered
                         : k == "gyro_on" ? K::GyroOn : K::GyroOff;
            const std::string name = a.size() > 2 ? a[2] : "Autotest Pad";
            runtime::input::push_controller_event(kind, 3, name.c_str(), "autotest", false);
            return "";
        }
        if (op == "shot" && a.size() == 2) {
            g.shot_path = a[1];
            return "";
        }
        if (op == "quit") {
            SDL_Event q{};
            q.type = SDL_QUIT;
            SDL_PushEvent(&q);
            return "";
        }
        return "unknown action " + op;
    }
    void Log(const std::string& line) override { std::cout << "[UIT14] " << line << std::endl; }
};

// "do auto_point_center 1": the game asks on its own, at a varying time at the title (dPadManager PointCenter after
// the MotionPlus calibration), to point the remote at the screen and press A; the pointer is held centred until
// then. Like a player, the autotest then presses A (left mouse) once the prompt has been up for 2 s.
void AutoPointCenter() {
    if (!g.at_auto_pc || !g.window) return;
    const double t = Now();
    if (g.at_pc_release > 0) {
        if (t >= g.at_pc_release) {
            SDL_SendMouseButton(g.window, 0, SDL_RELEASED, SDL_BUTTON_LEFT);
            g.at_pc_release = 0;
            g.at_pc_since = 0;
        }
        return;
    }
    if (!runtime::input::g_guest_point_center.load() || g.menu_open.load()) {
        g.at_pc_since = 0;
        return;
    }
    if (g.at_pc_since == 0) g.at_pc_since = t;
    if (t - g.at_pc_since < 2.0) return;
    SDL_SendMouseButton(g.window, 0, SDL_PRESSED, SDL_BUTTON_LEFT);
    g.at_pc_release = t + 0.25;
    std::cout << "[UIT14] auto: the game asks to point the remote at the screen; pressing A (left mouse)" << std::endl;
}

void AutotestTick() {
    if (!g.at || g.at->done()) return;
    static AppHost host;
    AutoPointCenter();
    g.at->Tick(host);
}

void SaveFboPng(GLuint fbo, int w, int h, const std::string& path, std::vector<std::string>& written);

// A pending autotest shot of `fbo` (w x h): written once.
void MaybeShot(GLuint fbo, int w, int h) {
    if (g.shot_path.empty()) return;
    std::vector<std::string> written;
    SaveFboPng(fbo, w, h, g.shot_path, written);
    std::cout << "[UIT14] shot " << (written.empty() ? "FAILED " : "") << g.shot_path << " (" << w << "x" << h << ")"
              << std::endl;
    g.shot_path.clear();
}

// An unattended autotest never leaves the launcher waiting for a player: a broken script, or a script that
// finished more than 2 s ago without starting the game, ends it (AFSLUITEN).
bool AutotestEndsLauncher() {
    if (!g.at_failed && !(g.at && g.at->done())) return false;
    if (!g.at_failed) {
        if (g.at_done_t == 0) g.at_done_t = Now();
        if (Now() - g.at_done_t < 2.0) return false;
    }
    std::cout << "[UIT14] autotest ended in the launcher: AFSLUITEN" << std::endl;
    return true;
}

bool RunLauncher() {
    const int prev_si = SDL_GL_GetSwapInterval();
    SDL_GL_SetSwapInterval(g.dwm_flush ? 0 : 1);
    bool play = true, running = true;
    g.page = Page::Main;
    g.focus_menu = true;
    g.in_launcher = true;
    while (running) {
        TickDiscordPresence();
        AutotestTick(); // UIT14 (no-op without NWII_UI_AUTOTEST)
        if (AutotestEndsLauncher()) {
            play = false;
            break;
        }
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (ProcessControlsEvent(e)) continue; // capture edits and reserved keys before menu shortcuts
            // F11 / Alt+Enter toggle fullscreen only: ImGui must not also see the Enter (UIT14: it activated
            // the focused entry, so Alt+Enter on the title screen started the game).
            if (e.type == SDL_KEYDOWN && IsFullscreenKey(e.key.keysym)) {
                if (!e.key.repeat) {
                    const bool fs_now = (SDL_GetWindowFlags(g.window) & SDL_WINDOW_FULLSCREEN) != 0;
                    SDL_SetWindowFullscreen(g.window, fs_now ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                }
                continue;
            }
            ImGui_ImplSDL2_ProcessEvent(&e);
            if (e.type == SDL_KEYDOWN) { g.dbg_key = e.key.keysym.sym; g.dbg_key_win = e.key.windowID; }
            if (e.type == SDL_QUIT ||
                (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE &&
                 e.window.windowID == SDL_GetWindowID(g.window))) {
                play = false;
                running = false;
            } else if (e.type == SDL_CONTROLLERBUTTONDOWN || e.type == SDL_CONTROLLERBUTTONUP ||
                       e.type == SDL_CONTROLLERAXISMOTION) {
                FeedGamepad(e);
            } else if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                TrackWindowSize();
            }
        }
        if (!running) break;
        DrainControllerEvents();
        if (SDL_GetWindowFlags(g.window) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(50);
            continue;
        }
        int ww = 0, wh = 0, dw = 0, dh = 0;
        SDL_GetWindowSize(g.window, &ww, &wh);
        SDL_GL_GetDrawableSize(g.window, &dw, &dh);
        NewUiFrame(float(ww), float(wh));
        const Action act = LauncherFrame(float(ww), float(wh), g.font_scale);
        ImGui::Render();
        EndUiFrame();
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, dw, dh);
        glClearColor(0.03f, 0.025f, 0.02f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        MaybeShot(0, dw, dh);
        SDL_GL_SwapWindow(g.window);
        if (g.dwm_flush) g.dwm_flush();
        if (act == Action::Play) running = false;
        if (act == Action::Quit) { play = false; running = false; }
    }
    // Leave a black window for the boot (the game's first frames take a moment).
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    SDL_GL_SwapWindow(g.window);
    SDL_GL_SetSwapInterval(prev_si);
    TrackWindowSize();
    g.in_launcher = false;
    std::cout << "[UI13] launcher: " << (play ? "SPELEN" : "AFSLUITEN") << std::endl;
    return play;
}

void ReleaseGl() {
    if (g.bg_tex) glDeleteTextures(1, &g.bg_tex);
    if (g.logo_tex) glDeleteTextures(1, &g.logo_tex);
    if (g.wing_tex) glDeleteTextures(1, &g.wing_tex); // LAUNCH19
    g.wing_tex = 0;
    if (g.ui_tex) glDeleteTextures(1, &g.ui_tex);
    if (g.ui_fbo) glDeleteFramebuffers(1, &g.ui_fbo);
    g.bg_tex = g.logo_tex = g.ui_tex = g.ui_fbo = 0;
}

void SaveFboPng(GLuint fbo, int w, int h, const std::string& path, std::vector<std::string>& written) {
    Image im;
    im.w = w;
    im.h = h;
    im.rgba.resize(size_t(w) * h * 4);
    // Also used inside the game's frame loop (UIT14 autotest shots): keep the renderer's bindings.
    GLint read_fbo = 0, pack_buf = 0, pack_align = 4;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buf);
    glGetIntegerv(GL_PACK_ALIGNMENT, &pack_align);
    if (pack_buf) glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, im.rgba.data());
    glPixelStorei(GL_PACK_ALIGNMENT, pack_align);
    if (pack_buf) glBindBuffer(GL_PIXEL_PACK_BUFFER, GLuint(pack_buf));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(read_fbo));
    std::vector<uint8_t> row(size_t(w) * 4);
    for (int y = 0; y < h / 2; ++y) {
        uint8_t* a = &im.rgba[size_t(y) * w * 4];
        uint8_t* b = &im.rgba[size_t(h - 1 - y) * w * 4];
        std::memcpy(row.data(), a, row.size());
        std::memcpy(a, b, row.size());
        std::memcpy(b, row.data(), row.size());
    }
    for (size_t i = 3; i < im.rgba.size(); i += 4) im.rgba[i] = 255;
    std::string err;
    if (SavePngWic(path, im, &err)) {
        written.push_back(path);
        std::cout << "[UI13] self-test wrote " << path << std::endl;
    } else {
        std::cout << "[UI13] self-test PNG failed: " << err << std::endl;
    }
}

} // namespace

// ============================================================================================ hooks
void OnConfigLoaded(const char* config_path, int argc, char** argv) {
    crashreport::Install(); // UIT14: NWII_CRASH_REPORT=1 (probe runs too)
    g.config_path = config_path ? config_path : "";
    for (int i = 1; i < argc; ++i)
        if (argv[i] && std::strcmp(argv[i], "--no-launcher") == 0) g.launcher_arg_skip = true;
    if (const char* e = std::getenv("NWII_NO_LAUNCHER"); e && *e == '1') g.launcher_arg_skip = true;
    if (const char* script = std::getenv("NWII_UI_AUTOTEST"); script && *script) {
        // UIT14: scripted launcher/menu run. Needs the UI: NWII_UI_FORCE=1 when a run limit is set.
        std::string text, err;
        if (FILE* f = std::fopen(script, "rb")) {
            char buf[4096];
            size_t n = 0;
            while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
            std::fclose(f);
            std::vector<autotest::Step> steps;
            if (autotest::Parse(text, steps, &err)) {
                double step_s = 0.12;
                if (const char* ms = std::getenv("NWII_UI_AUTOTEST_STEP_MS"); ms && *ms) step_s = std::atof(ms) / 1000.0;
                std::cout << "[UIT14] autotest: " << steps.size() << " steps from " << script
                          << (AutomatedRun() ? " (UI OFF in this run: set NWII_UI_FORCE=1)" : "") << std::endl;
                g.at = std::make_unique<autotest::Runner>(std::move(steps), step_s);
                g.at_cursor = true; // CTRL18-UI: no hover from the real cursor until the script moves its own
                g.at_x = g.at_y = -FLT_MAX;
            } else {
                std::cout << "[UIT14] autotest script error: " << err << std::endl;
                g.at_failed = true;
            }
        } else {
            std::cout << "[UIT14] autotest script not readable: " << script << std::endl;
            g.at_failed = true;
        }
    }
    if (AutomatedRun()) return;
    ApplyUserSettings(g.config_path);
    ApplyEnvFromConfig(Config::get());
    crashreport::ArmIfRequested(); // LAUNCH19: [diag] crash_report = true (NWII_CRASH_REPORT from the settings)
    g.settings_path = UserSettingsPath(g.config_path, Config::get());
    g.saved = g.edit = g.boot = g.boot_app = FromConfig(Config::get()); // LAUNCH19: what this SkyBreak started with
    g.log_dir = LogDir();
    const char* achievement_path = std::getenv("NWII_ACHIEVEMENTS_FILE");
    g.achievements.Load(achievement_path && *achievement_path ? U8(achievement_path) : U8(g.settings_path).parent_path()/"achievements.toml");
    g.achievements_loaded = true;
    { // LAUNCH19: [ui] remember_window - the values at start (Shutdown writes changes back)
        const runtime::UiConfig& u = runtime::ui_config();
        g.win_w0 = Config::get().window_width;
        g.win_h0 = Config::get().window_height;
        g.win_x0 = u.window_x;
        g.win_y0 = u.window_y;
        g.win_fs0 = Config::get().fullscreen;
    }
    // CTRL18-UI: Config::load ignores a file with a TOML error as a whole; tell the player which line.
    for (const std::string& f : {g.settings_path, g.config_path}) {
        const std::string p = SettingsFileProblem(f);
        if (p.empty()) continue;
        const std::string name = P8(U8(f).filename());
        g.settings_problem = "Let op: " + name + " kon niet gelezen worden (" + p + "). Het spel gebruikt voor dit "
                             "bestand nu de standaardwaarden.";
        if (f == g.settings_path)
            g.settings_problem += " Bij Opslaan wordt het oude bestand eerst bewaard als " + name + ".bak.";
        g.settings_problem = name + ", " + p; // LAUNCH19: short ("instellingen.toml, regel 12: [graphics] staat er twee keer in")
        std::cout << "[CTRL18-UI] settings file problem: " << f << ": " << p << std::endl;
        PushToast("INSTELLINGEN NIET GELEZEN", name + " " + p);
        break;
    }
}

bool Start(SDL_Window* window, SDL_GLContext gl_ctx, const char* game_dir) {
    if (AutomatedRun() || !window || !gl_ctx) return true;
    const bool launcher = runtime::ui_config().launcher && !g.launcher_arg_skip;
    if (!launcher && !runtime::ui_config().overlay) return true;
    g.game_dir = game_dir ? game_dir : "";
    if (!InitImGui(window, gl_ctx)) return true;
    SDL_SetWindowTitle(window, "SkyBreak"); // LAUNCH19 (was "Skyward Sword PC"; the icon comes with LoadLook)
    { // LAUNCH19: [ui] remember_window - the position of last time, when it is still on a display
        const runtime::UiConfig& u = runtime::ui_config();
        if (u.remember_window && u.window_x != INT_MIN && u.window_y != INT_MIN && !Config::get().fullscreen) {
            int ww = 0, wh = 0;
            SDL_GetWindowSize(window, &ww, &wh);
            for (int i = 0; i < SDL_GetNumVideoDisplays(); ++i) {
                SDL_Rect d{};
                SDL_GetDisplayUsableBounds(i, &d);
                if (u.window_x + 40 > d.x && u.window_x + 40 < d.x + d.w && u.window_y + 10 >= d.y && u.window_y + 10 < d.y + d.h) {
                    SDL_SetWindowPosition(window, u.window_x, u.window_y);
                    break;
                }
            }
        }
    }
    LoadLook();
    StartDiscCheck();
    for (int i = 0, n = SDL_GetNumAudioDevices(0); i < n && i < 32; ++i) // LAUNCH19: [audio] device choices
        if (const char* name = SDL_GetAudioDeviceName(i, 0)) g.audio_devices.push_back(name);
    RefreshSaveInfo(); // LAUNCH19: main screen summary
    if (launcher && !RunLauncher()) {
        Shutdown();
        return false;
    }
    { // LAUNCH19: before the game boots - backup of the save ([nand] backups, user decision: last 5), the audio device /
      // latency chosen in the launcher (the launcher music may have opened the device already), master volume
        const saves::Outcome o = saves::BackupAtStart(NandRoot(), runtime::ui_config().backups, En());
        g.backup_msg = o.message;
        std::cout << "[LAUNCH19] save backup: " << o.message << (o.backup.empty() ? "" : " (" + o.backup + ")") << std::endl;
        runtime::audio::apply_host_settings();
        SDL_SetWindowTitle(window, "SkyBreak \xE2\x80\x94 The Legend of Zelda: Skyward Sword");
    }
    if (runtime::ui_config().overlay) {
        g.overlay = true;
        runtime::input::InputManager::get().add_source(std::make_unique<CaptureSource>(&g.menu_open));
        std::cout << "[UI13] in-game menu ready (F1 / controller Mute-Share-Capture; Esc = the game's gear menu)" << std::endl; // CTRL18-UI
    }
    g.in_game = true;
    g.saved = g.edit = g.boot = FromConfig(Config::get()); // LAUNCH19: what the game boots with (restart chips in F1)
    g.focused = (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
    ApplyAudioGain(); // LAUNCH19: [audio] master_volume / mute_when_unfocused
    ApplyGlyphSet(g.saved); // CTRL18-UI: "auto" = the device used in the launcher (else a connected pad / keyboard)
    return true;
}

bool HandleEvent(const SDL_Event& e) {
    if (g.overlay && !ControlsCapturing() && e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F8 && !e.key.repeat) {
        g.report_pending = true; return true;
    }

    if (!g.overlay) return false;
    if (ProcessControlsEvent(e)) return true; // capture edits preview before drawing
    if (e.type == SDL_KEYDOWN) { g.dbg_key = e.key.keysym.sym; g.dbg_key_win = e.key.windowID; } // autotest query
    if (e.type == SDL_WINDOWEVENT && (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST || e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED)) {
        g.focused = e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED; // LAUNCH19: [audio] mute_when_unfocused
        ApplyAudioGain();
    }
    const bool open = g.menu_open.load();
    switch (RouteEvent(e, open)) {
    case Route::Pass:
        return false;
    case Route::Toggle:
        if (open && g.confirm_quit) g.confirm_quit = false;
        else if (open && g.page != Page::Main) { RevertEdits(); g.pst.Open(); g.page = Page::Main; g.focus_menu = true; } // LAUNCH19
        else SetMenuOpen(!open);
        return true;
    case Route::Menu:
        if (e.type == SDL_CONTROLLERBUTTONDOWN || e.type == SDL_CONTROLLERBUTTONUP || e.type == SDL_CONTROLLERAXISMOTION)
            FeedGamepad(e);
        else
            ImGui_ImplSDL2_ProcessEvent(&e);
        return true;
    case Route::Both:
        if (e.type != SDL_CONTROLLERDEVICEADDED && e.type != SDL_CONTROLLERDEVICEREMOVED) ImGui_ImplSDL2_ProcessEvent(&e);
        return false;
    }
    return false;
}

unsigned ComposeFrame(unsigned present_fbo, int pw, int ph) {
    AutotestTick(); // UIT14 (no-op without NWII_UI_AUTOTEST)
    TickAchievements();
    TickDiscordPresence();
    if (g.report_pending && present_fbo && pw > 0 && ph > 0) CaptureReport(present_fbo, pw, ph);
    if (g.overlay) { // CTRL18-UI: one-time gyro aiming hint (a gyro pad, "Switch HD", gyro aiming off)
        const std::string tip = ControlsTakeGyroHint(g.saved);
        if (!tip.empty()) {
            PushToast("TIP", tip);
            runtime::ui_config().gyro_hint_shown = true;
            std::string err;
            runtime::write_user_settings(g.settings_path, {{"ui", "gyro_hint_shown", "true"}}, &err);
        }
    }
    PerfTick(); // LAUNCH19
    const bool perf = runtime::ui_config().perf_overlay != "off" && !runtime::ui_config().perf_overlay.empty();
    if (!g.overlay || (!g.menu_open.load(std::memory_order_relaxed) && g.toasts.empty() && !perf)) {
        if (g.overlay && present_fbo && pw > 0 && ph > 0) MaybeShot(present_fbo, pw, ph);
        return present_fbo;
    }
    if (!present_fbo || pw <= 0 || ph <= 0) return present_fbo;
    MenuMouseGuard();
    ImVec2 a, b;
    PictureRect(a, b);
    int ww = 0, wh = 0;
    WindowSize(ww, wh);
    GLint draw_fbo = 0, read_fbo = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    EnsureUiFbo(pw, ph);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, present_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g.ui_fbo);
    if (scissor) glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0, 0, pw, ph, 0, 0, pw, ph, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(draw_fbo));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(read_fbo));
    NewUiFrame(float(ww), float(wh));
    OverlayFrame(a, b, g.font_scale);
    ImGui::Render();
    EndUiFrame();
    RenderInto(g.ui_fbo, pw, ph, a, b);
    MaybeShot(g.ui_fbo, pw, ph);
    g.last_compose = Now();
    return g.ui_fbo;
}

bool WantsPresent() {
    AutotestTick(); // UIT14 (no-op without NWII_UI_AUTOTEST)
    if (!g.overlay) return false;
    if (g.report_pending) return true;
    DrainControllerEvents(); // once per host-loop iteration: an uncontended mutex, nothing else
    MenuMouseGuard();
    if (!g.menu_open.load(std::memory_order_relaxed) && g.toasts.empty()) return false;
    return Now() - g.last_compose >= 1.0 / 30.0;
}

bool MenuOpen() { return g.menu_open.load(std::memory_order_relaxed); }

void Shutdown() {
    runtime::discord::Shutdown();
    crashreport::MarkTeardown(); // UIT14: main's teardown has started (NWII_CRASH_REPORT phase)
    if (g.at && !g.at->done())
        std::cout << "[UIT14] RESULT pass=" << g.at->passed() << " fail=" << g.at->failed()
                  << (g.at->next_step() < g.at->step_count()
                          ? " INCOMPLETE (the game ended at step " + std::to_string(g.at->next_step()) + "/" +
                                std::to_string(g.at->step_count()) + ")"
                          : " (the game ended with the last step)")
                  << std::endl;
    g.at.reset();
    if (g.disc_thread.joinable()) g.disc_thread.join();
    // LAUNCH19: [ui] remember_window - size, position and fullscreen back into the settings file when they changed
    if (g.imgui && g.window && !g.selftest && !AutomatedRun() && runtime::ui_config().remember_window && !g.settings_path.empty()) {
        int x = 0, y = 0, w = 0, h = 0;
        const bool fsn = (SDL_GetWindowFlags(g.window) & SDL_WINDOW_FULLSCREEN) != 0;
        SDL_GetWindowPosition(g.window, &x, &y);
        SDL_GetWindowSize(g.window, &w, &h);
        std::vector<runtime::SettingEdit> e;
        if (fsn != g.win_fs0) e.push_back({"graphics", "fullscreen", fsn ? "true" : "false"});
        if (!fsn && w > 0 && h > 0 && (w != g.win_w0 || h != g.win_h0)) {
            e.push_back({"graphics", "window_width", std::to_string(w)});
            e.push_back({"graphics", "window_height", std::to_string(h)});
        }
        if (!fsn && (x != g.win_x0 || y != g.win_y0)) {
            e.push_back({"graphics", "window_x", std::to_string(x)});
            e.push_back({"graphics", "window_y", std::to_string(y)});
        }
        std::string err;
        if (!e.empty() && SettingsFileProblem(g.settings_path).empty() && runtime::write_user_settings(g.settings_path, e, &err))
            std::cout << "[LAUNCH19] window remembered (" << w << "x" << h << " at " << x << "," << y << (fsn ? ", fullscreen" : "")
                      << ")" << std::endl;
    }
    if (!g.imgui) return;
    g.overlay = false;
    g.menu_open.store(false);
    ReleaseGl();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    g.imgui = false;
}

// ============================================================================================ self-test
int MaybeRunSelfTest(const char* game_dir) {
    const char* out = std::getenv("NWII_UI_SELFTEST");
    if (!out || !*out) return -1;
    int W = 1280, H = 720;
    if (const char* sz = std::getenv("NWII_UI_SELFTEST_SIZE"); sz && *sz) std::sscanf(sz, "%dx%d", &W, &H);
    std::cout << "[UI13] self-test " << W << "x" << H << " -> " << out << std::endl;
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        std::cout << "[UI13] self-test: SDL_Init failed: " << SDL_GetError() << std::endl;
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_Window* win = SDL_CreateWindow("UI13 self-test", 0, 0, W, H, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    SDL_GLContext ctx = win ? SDL_GL_CreateContext(win) : nullptr;
    if (!ctx || !gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)) {
        std::cout << "[UI13] self-test: no GL context: " << SDL_GetError() << std::endl;
        return 1;
    }
    g.selftest = true;
    g.force_w = W;
    g.force_h = H;
    g.game_dir = game_dir ? game_dir : "";
    if (g.settings_path.empty()) g.settings_path = UserSettingsPath(g.config_path, Config::get());
    g.saved = g.edit = FromConfig(Config::get());
    if (!InitImGui(win, ctx)) return 1;
    LoadLook();
    g.disc = CheckDisc(g.game_dir);
    g.disc_done.store(true);
    // A sample toast as on a real start with a DualSense connected.
    g.toasts.push_back({"CONTROLLER VERBONDEN", "DualSense Wireless Controller", Now() - 1.0});
    std::string base = out;
    if (base.size() > 4 && base.substr(base.size() - 4) == ".png") base.resize(base.size() - 4);
    GLuint fbo = 0, tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    std::vector<std::string> written;
    const auto launcher_shot = [&](const std::string& path, int frames) {
        for (int i = 0; i < frames; ++i) {
            NewUiFrame(float(W), float(H));
            LauncherFrame(float(W), float(H), g.font_scale);
            ImGui::Render();
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glViewport(0, 0, W, H);
            glClearColor(0.03f, 0.025f, 0.02f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        }
        SaveFboPng(fbo, W, H, path, written);
    };
    // LAUNCH19: the main screen and every settings tab (the same scenes as the LAUNCH19 mockups in
    // pc-testresultaten/launcher19), the unsaved-changes dialog and the controls sub page
    g.window = win;
    RefreshSaveInfo();
    g.page = Page::Main;
    launcher_shot(base + "-01-hoofdscherm.png", 30);
    g.page = Page::Achievements;
    launcher_shot(base + "-12-achievements.png", 4);
    g.page = Page::Main;
    const Settings start = g.edit;
    struct Scene { int tab; const char* focus; const char* file; };
    const Scene scenes[] = {{0, "game_dir", "-02-start"},           {1, "internal_scale", "-03-beeld"},
                            {3, "gyro_aim", "-04-besturing"},       {5, "background_video", "-05-interface"},
                            {4, "a_does_not_dash", "-08-gameplay"}, {6, "crash_report", "-09-geavanceerd"},
                            {2, "master_volume", "-10-geluid"}};
    g.page = Page::Settings;
    for (const Scene& sc : scenes) {
        g.pst.Open();
        g.pst.tab = sc.tab;
        g.pst.focus[sc.tab] = sc.focus;
        g.edit = start;
        if (sc.tab == 1) { // two unsaved edits, one needs a restart (VSync)
            g.edit.internal_scale = start.internal_scale == 3 ? 2 : 3;
            g.edit.vsync = !start.vsync;
        }
        launcher_shot(base + sc.file + ".png", 3);
    }
    g.pst.Open();
    g.pst.tab = 1;
    g.pst.focus[1] = "internal_scale";
    g.edit.internal_scale = start.internal_scale == 3 ? 2 : 3;
    g.edit.vsync = !start.vsync;
    g.pst.dialog = page::Dialog::Unsaved;
    launcher_shot(base + "-07-niet-opgeslagen.png", 3);
    g.pst.Open();
    g.edit = start;
    g.pst.tab = 3;
    g.pst.sub_controls = true;
    launcher_shot(base + "-04b-besturing-knoppen.png", 3);
    g.pst.Open();
    runtime::ui_config().language = "en"; // English UI (catalog EN texts)
    g.pst.tab = 2;
    g.pst.focus[2] = "master_volume";
    launcher_shot(base + "-10b-geluid-english.png", 3);
    runtime::ui_config().language = start.ui_language;
    g.edit = start;
    // In-game menu over a stand-in game picture: present image 1280x960 (2x), shown 16:9 in the
    // window, exactly the ComposeFrame path.
    {
        const int pw = 1280, ph = 960;
        GLuint pfbo = 0, ptex = 0;
        glGenTextures(1, &ptex);
        glBindTexture(GL_TEXTURE_2D, ptex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, pw, ph, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glGenFramebuffers(1, &pfbo);
        glBindFramebuffer(GL_FRAMEBUFFER, pfbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ptex, 0);
        // stand-in picture: the background frame drawn by ImGui into the present image
        NewUiFrame(float(W), float(H));
        if (g.bg_tex)
            ImGui::GetBackgroundDrawList()->AddImage(ImTextureID(intptr_t(g.bg_tex)), ImVec2(0, 0), ImVec2(float(W), float(H)));
        ImGui::Render();
        RenderInto(pfbo, pw, ph, ImVec2(0, 0), ImVec2(float(W), float(H)));
        g.window = win;
        g.overlay = true;
        g.in_game = true;
        runtime::input::g_present_rect[0].store(0.0f);
        runtime::input::g_present_rect[1].store(0.0f);
        runtime::input::g_present_rect[2].store(1.0f);
        runtime::input::g_present_rect[3].store(1.0f);
        g.menu_open.store(true);
        g.page = Page::Main;
        g.focus_menu = true;
        unsigned shown = pfbo;
        for (int i = 0; i < 3; ++i) shown = ComposeFrame(pfbo, pw, ph);
        // what the window shows: the composite stretched into the 16:9 window
        glBindFramebuffer(GL_READ_FRAMEBUFFER, shown);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
        glBlitFramebuffer(0, 0, pw, ph, 0, 0, W, H, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        SaveFboPng(fbo, W, H, base + "-menu.png", written);
        if (shown == pfbo) std::cout << "[UI13] self-test: ComposeFrame returned the game image (menu not drawn)\n";
        // LAUNCH19: F1 > INSTELLINGEN > BEELD in game: launcher-only rows hidden, boot-time edits get "na herstart"
        g.boot = g.boot_app = g.saved;
        g.page = Page::Settings;
        g.pst.Open();
        g.pst.tab = 1;
        g.pst.focus[1] = "internal_scale";
        g.edit = g.saved;
        g.edit.internal_scale = g.saved.internal_scale == 3 ? 2 : 3;
        for (int i = 0; i < 3; ++i) shown = ComposeFrame(pfbo, pw, ph);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, shown);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
        glBlitFramebuffer(0, 0, pw, ph, 0, 0, W, H, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        SaveFboPng(fbo, W, H, base + "-06-ingame-f1-beeld.png", written);
        g.edit = g.saved;
        g.page = Page::Main;
        g.menu_open.store(false);
        g.toasts.clear();
        const unsigned closed = ComposeFrame(pfbo, pw, ph);
        std::cout << "[UI13] self-test: menu closed -> ComposeFrame " << (closed == pfbo ? "passes the game image through" : "DREW (unexpected)") << std::endl;
        glDeleteFramebuffers(1, &pfbo);
        glDeleteTextures(1, &ptex);
    }
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
    const bool ok = written.size() == 14; // LAUNCH19: main + 7 tabs + dialog + controls + English + menu + F1 settings
    Shutdown();
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    std::cout << "[UI13] self-test " << (ok ? "OK" : "FAILED") << " (" << written.size() << " PNGs)" << std::endl;
    return ok ? 0 : 1;
}

} // namespace nwii::ui
