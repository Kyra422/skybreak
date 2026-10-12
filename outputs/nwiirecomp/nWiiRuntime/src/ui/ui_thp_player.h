#pragma once
// THPVID19: the launcher's moving background (design: work/claude-notes/THPVID19-design.md). A film from the player's
// own disc (default THP/Demo01_02.thp, the game's title-screen demo film) plays in a loop behind the launcher, with
// its music through platform/audio_out. Own code only; the film is read at run time, nothing from the game ships.
//
// ui_app.cpp calls ThpBackgroundFrame() once per launcher frame, as the first statement of DrawBackdrop (the one marked
// THPVID19 hook). A decode thread (ThpSession: WIC JPEG -> BGRA, 6-frame ring, audio FIFO) works ahead; the hook
// uploads a frame only when a new one is due. No hook call (minimised, window drag, the game booted) = the clock
// stops and the music runs out; after 2 s without a call the thread frees everything and ends, and a later call
// resumes at the same place. Any failure keeps the still frame that UI13's LoadLook uploaded.
//
// Settings ([ui], runtime/ui_config.h; keys = LAUNCH19's catalog): background_video (true), background_music (true),
// background_music_volume (0..100, 60; gain = (v/100)^2), background_video_file ("file.thp[:N]", "" = the film of
// `background`, starting at its still) and the existing background = "file.thp[:N]" (the still while the video is off).
// The music is muted (0.3 s fade) while the launcher window is inactive and stops when the game starts (no more hook
// calls). Env: NWII_UI_VIDEO=0|1 (1 also under NWII_UI_AUTOTEST / NWII_UI_SELFTEST, where it is off by default),
// NWII_UI_MUSIC=0|1, NWII_UI_MUSIC_VOLUME=0..100 (old name NWII_UI_VIDEO_VOLUME), NWII_UI_VIDEO_FILE=<file[:N]>,
// NWII_UI_BACKGROUND=<spec>, NWII_UI_VIDEO_STATS=1 (a status line every 10 s with the set and the measured music gain;
// =N: every N s, 2..600). No launcher music while NWII_AUDIO_DUMP is set.
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "ui_thp.h"

namespace nwii::ui {

// Seconds on a monotonic clock (steady_clock); the hook and the decode thread share it.
double ThpNow();

// ------------------------------------------------------------------------------------------------ the hook (GL thread)
// tex/w/h = ui_app's backdrop texture and its DRAW size. While the film plays the current frame is uploaded into `tex`
// (created when 0; ui_app keeps ownership and deletes it in ReleaseGl). w/h become the display size: the 608x464 films
// are 16:9 anamorphic -> 811x464, also for the still when the video is off. True when `tex` holds a video frame.
bool ThpBackgroundFrame(const std::string& game_dir_utf8, unsigned int& tex, int& w, int& h);

// LAUNCH19 live preview: the INTERFACE page passes its unsaved values every frame while it is open; nullptr = use
// runtime::ui_config() again (after Save or Cancel). GL/main thread only.
using ThpBackgroundPrefs = thp::BackgroundPrefs;
void ThpBackgroundPreview(const ThpBackgroundPrefs* draft);

// Optional (Quit path): stop the decode thread and wait for it (at most 1 s). Safe when it never started.
void ThpBackgroundShutdown();

struct ThpBackgroundStats {
    bool active = false;       // a decode thread exists (starting or playing)
    bool playing = false;      // frames are being shown
    bool failed = false;
    uint64_t shown = 0, decoded = 0, dropped = 0, errors = 0, k = 0;
    double decode_ms = 0, t = 0;
    bool audio_clock = false;
};
ThpBackgroundStats ThpBackgroundGetStats();

// ------------------------------------------------------------------------------------------------ decode thread
// One film being played: its own worker thread (below-normal priority, COM MTA, one WIC factory). Used by the hook;
// exposed for the unit test (outputs/pc-tests/thpvid19), which drives it without GL.
struct ThpSessionParams {
    std::string path;              // UTF-8 path of the .thp file
    std::string file;              // its name as in the spec (selects the built-in loop plan)
    int still = 0;                 // first-pass start (thp::ResolveFilm: ":N", else the still frame of the same film)
    uint64_t k0 = 0;               // first sequence number (resume point)
    bool music = false;            // decode audio into the FIFO from k0
    uint32_t audio_fifo_ms = 480;  // FIFO target (latency target + 400 ms)
    double idle_timeout = 2.0;     // seconds without Touch() -> stop and free everything
    int slots = 6;                 // video ring
};

class ThpSession {
public:
    enum class State : int { Starting = 0, Playing = 1, Stopped = 2, Failed = 3 };
    struct Stats {
        uint64_t decoded = 0, dropped = 0, errors = 0, shown = 0, audio_blocks = 0;
        double decode_ms_avg = 0;
    };
    // Starts the worker thread (detached; it holds its own reference).
    static std::shared_ptr<ThpSession> Start(const ThpSessionParams& p);
    ~ThpSession();

    State state() const { return State(state_.load()); }
    std::string fail_reason() const;
    // Valid once state() != Starting.
    const thp::MovieInfo& info() const { return info_; }
    const thp::LoopPlan& plan() const { return plan_; }
    const ThpSessionParams& params() const { return p_; }

    void Touch(double now) { last_call_.store(now); }  // the hook ran (idle watchdog)
    void PublishClock(double t) { clock_t_.store(t); } // media time (late frames are skipped, not decoded)
    // Newest ready frame with pts <= t that is newer than the last one handed over: upload(bgra, w, h, k) runs under
    // the session lock; the frame (and every older ready one) is released afterwards. False = nothing new.
    bool TakeFrame(double t, const std::function<bool(const uint8_t*, int, int, uint64_t)>& upload);
    bool any_shown() const { return any_shown_.load(); }
    uint64_t shown_k() const { return shown_k_.load(); }

    // Music on/off; on = the FIFO restarts at seek_k (fade-in). Block = one THP frame of interleaved stereo s16.
    void SetMusic(bool on, uint64_t seek_k);
    bool PopAudio(std::vector<int16_t>& lr, uint64_t* k = nullptr);
    size_t audio_frames_queued() const;

    void RequestStop();
    bool WaitStopped(double timeout_s); // true when the worker has ended
    Stats stats() const;
    size_t bytes_held() const;          // video + audio buffers the worker owns now (0 after it ended)

private:
    ThpSession() = default;
    void Run();

    ThpSessionParams p_;
    std::atomic<int> state_{0};
    std::atomic<double> last_call_{0}, clock_t_{0};
    std::atomic<bool> stop_{false}, ended_{false}, any_shown_{false};
    std::atomic<uint64_t> shown_k_{0};
    std::atomic<uint64_t> decoded_{0}, dropped_{0}, errors_{0}, shown_{0}, audio_blocks_{0};
    std::atomic<double> decode_ms_sum_{0};
    std::atomic<size_t> bytes_{0};
    thp::MovieInfo info_;
    thp::LoopPlan plan_;

    mutable std::mutex mu_;
    std::condition_variable cv_, ended_cv_;
    struct Slot {
        uint64_t k = 0;
        double pts = 0;
        int st = 0; // 0 free, 1 decoding (worker), 2 ready
        std::vector<uint8_t> bgra;
    };
    std::vector<Slot> slots_;
    double last_taken_pts_ = -1e300;
    struct Block {
        uint64_t k = 0;
        std::vector<int16_t> lr;
    };
    std::vector<Block> fifo_; // small (<= ~20 blocks): a vector used as a queue
    size_t fifo_frames_ = 0;
    bool music_ = false;
    uint64_t audio_seek_k_ = UINT64_MAX;
    std::string fail_reason_;
};

} // namespace nwii::ui
