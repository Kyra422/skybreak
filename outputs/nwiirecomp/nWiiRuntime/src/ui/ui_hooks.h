#pragma once
// UI13: the hooks main.cpp calls (launcher, settings, in-game menu, toasts). Every hook is a
// no-op for automated runs (NWII_RUN_SECONDS / NWII_SS_INPUT_REPLAY / NWII_HEADLESS) and when
// the runtime is built without the UI (NWII_HAVE_UI undefined, e.g. a build without ImGui).
#include <SDL.h>

namespace nwii::ui {

#ifdef NWII_HAVE_UI
// Right after the config is loaded: lays the player's saved settings over the profile
// (ui_settings.h), sets NWII_PRESENT_PACING / NWII_AUDIO_LATENCY_MS from [ui], reads
// --no-launcher.
void OnConfigLoaded(const char* config_path, int argc, char** argv);
// NWII_UI_SELFTEST=<file.png>: render the launcher, settings and in-game menu off-screen into
// PNGs and exit (hidden window). Returns -1 when not requested, else the process exit code.
int MaybeRunSelfTest(const char* game_dir);
// After the window and GL context exist, before the guest boots: the launcher (unless skipped)
// and the in-game menu set-up. False = the player chose QUIT (exit without booting).
bool Start(SDL_Window* window, SDL_GLContext gl_ctx, const char* game_dir);
// Every SDL event of the host loop, first. True = the menu took it (skip main.cpp's handling).
bool HandleEvent(const SDL_Event& e);
// Present composition: returns present_fbo itself (menu closed, no toast: no GL work) or a UI
// framebuffer of the same size with the menu/toasts drawn over the game picture.
unsigned ComposeFrame(unsigned present_fbo, int pw, int ph);
// True while the menu/toasts need a redraw although the game produced no new frame.
bool WantsPresent();
// The in-game menu is open (game input suspended).
bool MenuOpen();
// Before the GL context goes away.
void Shutdown();
#else
inline void OnConfigLoaded(const char*, int, char**) {}
inline int MaybeRunSelfTest(const char*) { return -1; }
inline bool Start(SDL_Window*, SDL_GLContext, const char*) { return true; }
inline bool HandleEvent(const SDL_Event&) { return false; }
inline unsigned ComposeFrame(unsigned present_fbo, int, int) { return present_fbo; }
inline bool WantsPresent() { return false; }
inline bool MenuOpen() { return false; }
inline void Shutdown() {}
#endif

} // namespace nwii::ui
