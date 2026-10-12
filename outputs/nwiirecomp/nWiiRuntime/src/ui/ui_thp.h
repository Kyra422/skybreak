#pragma once
// THPVID19: the launcher's moving background, pure core (no SDL/GL/WIC/ImGui, unit-tested by outputs/pc-tests/thpvid19).
// Reads THP movies (Nintendo's JPEG + DSP-ADPCM container) from the player's own disc at run time; nothing from the game
// is shipped. Design: work/claude-notes/THPVID19-design.md. The player (decode thread, GL upload, audio) is
// ui_thp_player.h.
//
// THP v1.1 layout (big endian): 0x30 header {magic "THP\0", version, bufSize, audioMaxSamples, fps f32, numFrames,
// firstFrameSize, movieDataSize, compInfoOffset, offsetDataOffset, movieDataOffset, finalFrameOffset}; component table
// {numComponents, 16 type bytes (0 video, 1 audio, 0xFF none), per component: video {w, h[, videoType]} / audio
// {channels, rate, samples[, tracks]}}; frames chained by "next frame size": {nextTotalSize, prevTotalSize,
// compSize[n]} + the component data in table order.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace nwii::ui::thp {

struct FrameEntry {
    uint64_t offset = 0;                   // file offset of the frame header
    uint32_t size = 0;                     // whole frame (header + components)
    uint32_t video_off = 0, video_size = 0; // relative to the frame start; size 0 = none
    uint32_t audio_off = 0, audio_size = 0;
};

struct MovieInfo {
    uint32_t version = 0, frames = 0, buf_size = 0, width = 0, height = 0;
    float fps = 0;
    uint32_t audio_channels = 0, audio_rate = 0, audio_samples = 0; // 0 channels = no audio
    uint64_t file_size = 0;
};

// One open THP file (own FILE*, opened from a UTF-8 path). Open walks the whole frame chain once and validates it
// (frame size >= 8 + 4n and <= bufSize, every component inside its frame, the chain inside the file). A chain that
// leaves the file (truncated copy) keeps the frames before that point when at least one is whole.
class Movie {
public:
    Movie() = default;
    Movie(const Movie&) = delete;
    Movie& operator=(const Movie&) = delete;
    ~Movie() { Close(); }
    bool Open(const std::string& path_utf8, std::string* err = nullptr);
    void Close();
    bool is_open() const { return f_ != nullptr; }
    const MovieInfo& info() const { return info_; }
    const std::vector<FrameEntry>& index() const { return index_; }
    bool truncated() const { return truncated_; } // the chain ended early (index shorter than the header count)
    // Whole frame i (header + components) / only its audio record.
    bool ReadFrame(uint32_t i, std::vector<uint8_t>& buf);
    bool ReadAudio(uint32_t i, std::vector<uint8_t>& buf);
    bool ReadVideo(uint32_t i, std::vector<uint8_t>& buf); // the raw (unstuffed) THP JPEG of frame i

private:
    std::FILE* f_ = nullptr;
    MovieInfo info_;
    std::vector<FrameEntry> index_;
    bool truncated_ = false;
};

// The raw video bytes of a THP frame -> a standard JPEG (THP leaves the 0xFF bytes of the scan unstuffed;
// ui::RestoreJpegStuffing gives every one its 0x00 back).
bool VideoJpeg(const uint8_t* raw, size_t n, std::vector<uint8_t>& jpeg, std::string* err = nullptr);

// One audio record (Nintendo THPAudioRecordHeader: offsetNextChannel, numSamples, 8 coefficient pairs per channel,
// yn1/yn2 per channel, then the DSP-ADPCM data per channel) -> interleaved stereo s16 (mono duplicated). Game
// semantics (THPAudioDecode: +0x400 rounding, history from the record header each frame). Returns samples per
// channel; 0 on a malformed record. hist_out (optional, [ch][0]=yn1 [ch][1]=yn2 after the last sample) and
// hist_in (the header's yn1/yn2) feed the continuity self-check of the unit test.
uint32_t DecodeAudio(const uint8_t* rec, size_t n, uint32_t channels, std::vector<int16_t>& lr,
                     int16_t hist_out[2][2] = nullptr, int16_t hist_in[2][2] = nullptr);
namespace detail {
// variant 0 = DecodeAudio; 1 = no +0x400 rounding, 2 = coefficient pair swapped (negative controls of the test only).
uint32_t DecodeAudioVariant(const uint8_t* rec, size_t n, uint32_t channels, std::vector<int16_t>& lr,
                            int16_t hist_out[2][2], int16_t hist_in[2][2], int variant);
}

// Sequence number k (continuous across loops) -> movie frame and fades. First pass [start, loop_out), then
// [loop_in, loop_out) forever. The last `xfade` frames before loop_out cross-fade into the loop_in picture while the
// audio fades out (equal power); audio fades in over `afade_in_first` frames from the (re)start frame and over
// `afade_in_wrap` frames after every wrap.
struct LoopPlan {
    uint32_t start = 0, loop_in = 0, loop_out = 1;
    uint32_t xfade = 30, afade_in_first = 30, afade_in_wrap = 10;
    uint32_t FrameAt(uint64_t k) const;
    float VideoMix(uint64_t k) const;                                // 0 = movie frame .. <1 = nearly the loop_in frame
    float AudioGain(uint64_t k, float pos, uint64_t k_start = 0) const; // pos 0..1 inside frame k; k_start = (re)start
    bool Valid() const { return loop_in < loop_out && start < loop_out; }
};
// Built-in plans: Demo01_02 (2859 frames, the title-screen demo film) loops [0, 2629) - frame 2630 on is the emblem/logo,
// the launcher shows its own wordmark; Demo01_01 (the prologue) starts and loops from its first non-black frame.
// Anything else loops the whole file. still_frame = the first-pass start (the still that showed before), clamped.
inline constexpr uint32_t kDemo0102Frames = 2859, kDemo0102LoopOut = 2629;
inline constexpr uint32_t kDemo0101Start = 248; // first frame with mean luma > 16 (measured; unit test V4b)
LoopPlan DefaultPlan(const std::string& file, const MovieInfo& info, int still_frame);

// "[ui] background" spec "file.thp[:N]" ("" = "Demo01_02.thp:400"); the same rules as UI13's LoadLook (no colon = 0).
void ParseBackgroundSpec(const std::string& spec, std::string& file, int& still_frame);

// Media time in seconds of the movie (pts of sequence number k = k / fps). Wall clock: each tick adds the real time
// since the previous tick, at most 0.1 s (no launcher frame = no time). Audio clock: the consumed samples of
// platform/audio_out (stats().played) since the first sample after StartAudio; never runs backwards.
class MediaClock {
public:
    void Reset(double t);                      // wall clock at t
    void StartAudio(double t_first_sample);    // audio clock: holds until `played` first advances, then t_first + d/rate
    void TickWall(double now);
    // expect_progress: the output ring holds at least its latency target, so the device should be consuming.
    void TickAudio(uint64_t played, uint32_t rate, double now, bool expect_progress);
    bool AudioStalled(double now) const; // played unchanged >= 0.5 s while expected to progress (ticks < 0.1 s apart)
    bool audio() const { return audio_; }
    double t() const { return t_; }

private:
    double t_ = 0, last_now_ = -1;
    bool audio_ = false, base_set_ = false, have_played0_ = false;
    double base_t_ = 0, stall_since_ = -1;
    uint64_t played0_ = 0, last_played_ = 0;
};

// Index of the ready slot with the newest pts <= t and pts > after (the picture never goes backwards); -1 = none.
int PickSlot(const double* pts, const bool* ready, int n, double t, double after = -1e300);

// Resolved launcher background preferences: settings (or the settings page's unsaved draft) + env overrides.
// Keys = LAUNCH19's catalog (user decisions 9 Oct: video on, music on at 60 %).
struct BackgroundPrefs {
    bool video = true;  // [ui] background_video
    bool music = true;  // [ui] background_music
    int volume = 60;    // [ui] background_music_volume 0..100
    std::string file;   // [ui] background_video_file "file.thp[:N]" ("" = the film of `spec`)
    std::string spec;   // [ui] background ("" = Demo01_02.thp:400): the still, shown while the video is off
    bool MusicOn() const { return music && volume > 0; } // false = no audio pushes at all
    float Gain() const;                                  // sample gain: (volume/100)^2 (perceptual), 0 when off
};
struct PrefsInputs {
    BackgroundPrefs config;                    // runtime::ui_config()
    const BackgroundPrefs* draft = nullptr;    // ThpBackgroundPreview (LAUNCH19 INTERFACE page), wins over config
    const char* env_video = nullptr;           // NWII_UI_VIDEO=0|1 (1 also under autotest/selftest)
    const char* env_music = nullptr;           // NWII_UI_MUSIC=0|1
    const char* env_volume = nullptr;          // NWII_UI_MUSIC_VOLUME=0..100 (old name NWII_UI_VIDEO_VOLUME still read)
    const char* env_file = nullptr;            // NWII_UI_VIDEO_FILE=<file[:N]>
    const char* env_background = nullptr;      // NWII_UI_BACKGROUND=<spec> (the still)
    bool ui_test = false;                      // NWII_UI_AUTOTEST / NWII_UI_SELFTEST set: video off unless NWII_UI_VIDEO=1
    bool audio_dump = false;                   // NWII_AUDIO_DUMP set: no launcher music (dumps stay pure game audio)
};
BackgroundPrefs ResolvePrefs(const PrefsInputs& in);

// The film to play. `file` "name.thp[:N]" when set: a plain file name in files/THP (no folder, no drive, ends in .thp),
// else the film of `spec`. First-pass start: an explicit ":N"; else the still frame when it is the still's film (no
// jump from the still); else 0 (the built-in plan's own start). False = `file` was refused (the still's film is used).
bool ResolveFilm(const BackgroundPrefs& pr, std::string& file, int& start);

// Focus mute: the music gain factor moves towards 1 (window active) or 0 (inactive / minimised) by dt / 0.3 s per
// launcher frame, dt capped at 0.1 s (a stalled frame never jumps the fade).
float StepFocusGain(float gain, bool focused, double dt);
// Scales one block of interleaved stereo s16 by a gain ramping linearly from g0 to g1 across the block (no click on a
// volume or focus change); adds the |source| and |result| sums to *in_abs / *out_abs (the measured gain, stats line).
void ApplyGainRamp(int16_t* lr, size_t frames, float g0, float g1, double* in_abs, double* out_abs);

// Display width of a movie frame: the 608x464 movies are 16:9 anamorphic (the game letterboxes them in 4:3), so their
// pixels are 4/3 wide -> 811x464. Other sizes keep square pixels.
int DisplayWidth(int w, int h);

} // namespace nwii::ui::thp
