// THPVID19: launcher background video - decode thread (ThpSession) and the GL-thread hook (see ui_thp_player.h).
// Own code only. Built with THPVID19_SESSION_ONLY (the unit test) the hook part is left out: no GL, SDL or audio_out.
#include "ui_thp_player.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#endif

#include <algorithm>
#include <cstdarg>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <thread>

#ifndef THPVID19_SESSION_ONLY
#include <SDL.h>
#include <glad/glad.h>

#include "../platform/audio_out.h"
#include "runtime/ui_config.h"
#include "ui_assets.h"
#include "ui_wic.h"
#endif

namespace nwii::ui {

double ThpNow() {
    using clk = std::chrono::steady_clock;
    static const clk::time_point t0 = clk::now();
    return std::chrono::duration<double>(clk::now() - t0).count();
}

namespace {
void Log(const std::string& line) { // one stdio call per line: no interleaving with other threads' lines
    const std::string s = "[THPVID19] " + line + "\n";
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fflush(stdout);
}

#ifdef _WIN32
// One WIC factory for the thread's life; JPEG -> 32bpp BGRA straight into the ring slot.
struct WicJpeg {
    IWICImagingFactory* f = nullptr;
    WicJpeg() {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) f = nullptr;
    }
    ~WicJpeg() {
        if (f) f->Release();
    }
    bool ok() const { return f != nullptr; }
    bool Decode(const std::vector<uint8_t>& jpg, uint8_t* dst, int w, int h) {
        if (!f || jpg.empty()) return false;
        IWICStream* ws = nullptr;
        IWICBitmapDecoder* d = nullptr;
        IWICBitmapFrameDecode* fr = nullptr;
        IWICFormatConverter* cv = nullptr;
        bool ok = SUCCEEDED(f->CreateStream(&ws)) &&
                  SUCCEEDED(ws->InitializeFromMemory(const_cast<BYTE*>(jpg.data()), DWORD(jpg.size()))) &&
                  SUCCEEDED(f->CreateDecoderFromStream(ws, nullptr, WICDecodeMetadataCacheOnDemand, &d)) &&
                  SUCCEEDED(d->GetFrame(0, &fr));
        if (ok) {
            UINT fw = 0, fh = 0;
            fr->GetSize(&fw, &fh);
            ok = int(fw) == w && int(fh) == h && SUCCEEDED(f->CreateFormatConverter(&cv)) &&
                 SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                          WICBitmapPaletteTypeCustom)) &&
                 SUCCEEDED(cv->CopyPixels(nullptr, UINT(w) * 4, UINT(size_t(w) * h * 4), dst));
        }
        if (cv) cv->Release();
        if (fr) fr->Release();
        if (d) d->Release();
        if (ws) ws->Release();
        return ok;
    }
};
#else
struct WicJpeg {
    bool ok() const { return false; }
    bool Decode(const std::vector<uint8_t>&, uint8_t*, int, int) { return false; }
};
#endif

std::string Fmt(const char* fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    return b;
}
} // namespace

// ------------------------------------------------------------------------------------------------ ThpSession
std::shared_ptr<ThpSession> ThpSession::Start(const ThpSessionParams& p) {
    std::shared_ptr<ThpSession> s(new ThpSession());
    s->p_ = p;
    s->p_.slots = std::clamp(p.slots, 2, 16);
    s->last_call_.store(ThpNow());
    try {
        std::thread([s] { s->Run(); }).detach();
    } catch (...) {
        s->fail_reason_ = "no decode thread";
        s->state_.store(int(State::Failed));
        s->ended_.store(true);
    }
    return s;
}

ThpSession::~ThpSession() = default;

std::string ThpSession::fail_reason() const {
    std::lock_guard<std::mutex> lk(mu_);
    return fail_reason_;
}

bool ThpSession::TakeFrame(double t, const std::function<bool(const uint8_t*, int, int, uint64_t)>& upload) {
    bool ok = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        const int n = int(slots_.size());
        if (n == 0) return false;
        double pts[16];
        bool ready[16];
        for (int i = 0; i < n; ++i) {
            pts[i] = slots_[i].pts;
            ready[i] = slots_[i].st == 2;
        }
        const int best = thp::PickSlot(pts, ready, n, t, last_taken_pts_);
        if (best < 0) return false;
        Slot& b = slots_[best];
        ok = upload(b.bgra.data(), int(info_.width), int(info_.height), b.k);
        last_taken_pts_ = b.pts;
        shown_k_.store(b.k);
        any_shown_.store(true);
        shown_.fetch_add(1);
        for (int i = 0; i < n; ++i) {
            if (slots_[i].st == 2 && slots_[i].pts <= last_taken_pts_) {
                if (i != best) dropped_.fetch_add(1); // decoded but overtaken by a newer frame before it was due
                slots_[i].st = 0;
            }
        }
    }
    cv_.notify_one();
    return ok;
}

void ThpSession::SetMusic(bool on, uint64_t seek_k) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (on == music_ && !on) return;
        music_ = on;
        fifo_.clear();
        fifo_frames_ = 0;
        audio_seek_k_ = on ? seek_k : UINT64_MAX;
    }
    cv_.notify_one();
}

bool ThpSession::PopAudio(std::vector<int16_t>& lr, uint64_t* k) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (fifo_.empty()) return false;
        lr.swap(fifo_.front().lr);
        if (k) *k = fifo_.front().k;
        fifo_frames_ -= std::min(fifo_frames_, lr.size() / 2);
        fifo_.erase(fifo_.begin());
    }
    cv_.notify_one();
    return true;
}

size_t ThpSession::audio_frames_queued() const {
    std::lock_guard<std::mutex> lk(mu_);
    return fifo_frames_;
}

void ThpSession::RequestStop() {
    stop_.store(true);
    cv_.notify_all();
}

bool ThpSession::WaitStopped(double timeout_s) {
    std::unique_lock<std::mutex> lk(mu_);
    return ended_cv_.wait_for(lk, std::chrono::duration<double>(timeout_s), [&] { return ended_.load(); });
}

ThpSession::Stats ThpSession::stats() const {
    Stats s;
    s.decoded = decoded_.load();
    s.dropped = dropped_.load();
    s.errors = errors_.load();
    s.shown = shown_.load();
    s.audio_blocks = audio_blocks_.load();
    s.decode_ms_avg = s.decoded ? decode_ms_sum_.load() / double(s.decoded) : 0.0;
    return s;
}

size_t ThpSession::bytes_held() const { return bytes_.load(); }

void ThpSession::Run() {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif
    std::string reason, why_stopped;
    // REVTHPVID19: an exception in here (std::filesystem on a non-UTF-8 name, bad_alloc) would leave std::thread's noexcept
    // entry = std::terminate = the whole process ends 0xC0000409 (the launcher, or the game in the 2 s after SPELEN).
    // Caught: the session ends as Failed and the hook keeps the still frame.
    try {
        WicJpeg wic;
        thp::Movie m;
        std::string err;
        if (!wic.ok()) reason = "WIC JPEG decoder unavailable";
        else if (!m.Open(p_.path, &err)) reason = err;
        else {
            info_ = m.info();
            plan_ = thp::DefaultPlan(p_.file, info_, p_.still);
            if (!plan_.Valid()) reason = "empty movie";
        }
        if (reason.empty()) {
            const int w = int(info_.width), h = int(info_.height);
            const size_t fb = size_t(w) * h * 4;
            const double fps = info_.fps;
            const bool has_audio = info_.audio_channels > 0;
            const size_t fifo_target = size_t(uint64_t(info_.audio_rate) * p_.audio_fifo_ms / 1000);
            {
                std::lock_guard<std::mutex> lk(mu_);
                slots_.resize(size_t(p_.slots));
                for (Slot& s : slots_) s.bgra.resize(fb);
                music_ = p_.music && has_audio;
                audio_seek_k_ = music_ ? p_.k0 : UINT64_MAX;
            }
            bytes_.store(fb * size_t(p_.slots));
            std::vector<uint8_t> raw, jpeg, arec;
            const auto decode = [&](uint32_t f, uint8_t* dst) {
                return m.ReadVideo(f, raw) && thp::VideoJpeg(raw.data(), raw.size(), jpeg) && wic.Decode(jpeg, dst, w, h);
            };
            // The crossfade target: the loop_in picture (Demo01_02: frame 0, white = the film's own opening).
            std::vector<uint8_t> xf;
            if (plan_.xfade) {
                xf.resize(fb);
                if (decode(plan_.loop_in, xf.data())) bytes_.fetch_add(fb);
                else xf.clear();
            }
            state_.store(int(State::Playing));
            Log(Fmt("playing %s from %u (%u frames, %ux%u, %.2f fps, audio %u/%u; loop %u..%u, k0=%llu)", p_.file.c_str(),
                    plan_.FrameAt(p_.k0), info_.frames, info_.width, info_.height, double(info_.fps),
                    info_.audio_channels, info_.audio_rate, plan_.loop_in, plan_.loop_out - 1,
                    (unsigned long long)p_.k0));
            uint64_t kv = p_.k0, ka = p_.k0, ka_start = p_.k0;
            int bad_in_row = 0;
            std::vector<int16_t> pcm;
            while (true) {
                if (stop_.load()) { why_stopped = "stopped"; break; }
                if (ThpNow() - last_call_.load() > p_.idle_timeout) {
                    why_stopped = Fmt("idle: stopped (no launcher frame for %.1f s)", p_.idle_timeout);
                    break;
                }
                bool worked = false;
                // --- audio: keep the FIFO at its target
                bool want_audio = false;
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    if (audio_seek_k_ != UINT64_MAX) {
                        ka = ka_start = audio_seek_k_;
                        audio_seek_k_ = UINT64_MAX;
                    }
                    want_audio = music_ && has_audio && fifo_frames_ < fifo_target;
                }
                if (want_audio) {
                    const uint64_t k = ka++;
                    const uint32_t f = plan_.FrameAt(k);
                    uint32_t n = 0;
                    if (m.ReadAudio(f, arec)) n = thp::DecodeAudio(arec.data(), arec.size(), info_.audio_channels, pcm);
                    if (n == 0) { // keep the timing: one frame of silence
                        n = uint32_t(std::lround(info_.audio_rate / fps));
                        pcm.assign(size_t(n) * 2, 0);
                        errors_.fetch_add(1);
                    }
                    const float g0 = plan_.AudioGain(k, 0.0f, ka_start), g1 = plan_.AudioGain(k, 1.0f, ka_start);
                    const float gm = plan_.AudioGain(k, 0.5f, ka_start);
                    if (g0 < 1.0f || g1 < 1.0f || gm < 1.0f) {
                        for (uint32_t i = 0; i < n; ++i) {
                            const float g = plan_.AudioGain(k, (float(i) + 0.5f) / float(n), ka_start);
                            pcm[2 * i] = int16_t(std::lround(pcm[2 * i] * g));
                            pcm[2 * i + 1] = int16_t(std::lround(pcm[2 * i + 1] * g));
                        }
                    }
                    {
                        std::lock_guard<std::mutex> lk(mu_);
                        if (music_ && audio_seek_k_ == UINT64_MAX) { // not re-seeked while decoding
                            fifo_.push_back(Block{k, pcm});
                            fifo_frames_ += n;
                            audio_blocks_.fetch_add(1);
                        }
                    }
                    worked = true;
                }
                // --- video: decode ahead into a free slot
                int slot = -1;
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    for (int i = 0; i < int(slots_.size()); ++i)
                        if (slots_[i].st == 0) { slot = i; slots_[i].st = 1; break; }
                }
                if (slot >= 0) {
                    const uint64_t k = kv++;
                    const double pts = double(k) / fps;
                    if (k > p_.k0 && pts < clock_t_.load() - 2.0 / fps) { // already late: skip without decoding
                        dropped_.fetch_add(1);
                        std::lock_guard<std::mutex> lk(mu_);
                        slots_[slot].st = 0;
                    } else {
                        uint8_t* dst;
                        {
                            std::lock_guard<std::mutex> lk(mu_);
                            dst = slots_[slot].bgra.data();
                        }
                        const auto t0 = std::chrono::steady_clock::now();
                        const bool ok = decode(plan_.FrameAt(k), dst);
                        if (ok) {
                            const float mix = xf.empty() ? 0.0f : plan_.VideoMix(k);
                            if (mix > 0.0f) {
                                const int a = std::clamp(int(mix * 256.0f + 0.5f), 0, 256);
                                for (size_t i = 0; i < fb; ++i) dst[i] = uint8_t((dst[i] * (256 - a) + xf[i] * a) >> 8);
                            }
                            decode_ms_sum_.store(decode_ms_sum_.load() +
                                                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
                            decoded_.fetch_add(1);
                            bad_in_row = 0;
                        } else {
                            errors_.fetch_add(1); // the previous picture stays
                            if (++bad_in_row >= 30) reason = "30 frames in a row did not decode";
                        }
                        std::lock_guard<std::mutex> lk(mu_);
                        slots_[slot].k = k;
                        slots_[slot].pts = pts;
                        slots_[slot].st = ok ? 2 : 0;
                    }
                    if (!reason.empty()) break;
                    worked = true;
                }
                if (!worked) {
                    std::unique_lock<std::mutex> lk(mu_);
                    cv_.wait_for(lk, std::chrono::milliseconds(250));
                }
            }
        }
    } catch (const std::exception& e) { // REVTHPVID19
        reason = std::string("exception: ") + e.what();
    } catch (...) {
        reason = "exception";
    }
#ifdef _WIN32
    if (SUCCEEDED(co)) CoUninitialize();
#endif
    const Stats st = stats();
    {
        std::lock_guard<std::mutex> lk(mu_);
        slots_.clear();
        slots_.shrink_to_fit();
        fifo_.clear();
        fifo_.shrink_to_fit();
        fifo_frames_ = 0;
        fail_reason_ = reason;
        bytes_.store(0);
        state_.store(int(reason.empty() ? State::Stopped : State::Failed));
        ended_.store(true);
    }
    ended_cv_.notify_all();
    if (!reason.empty())
        Log("video unavailable (" + reason + "): still frame");
    else
        Log(Fmt("%s; shown=%llu decoded=%llu dropped=%llu errors=%llu decode_ms=%.2f", why_stopped.c_str(),
                (unsigned long long)st.shown, (unsigned long long)st.decoded, (unsigned long long)st.dropped,
                (unsigned long long)st.errors, st.decode_ms_avg));
}

#ifndef THPVID19_SESSION_ONLY
// ------------------------------------------------------------------------------------------------ the hook (GL thread)
namespace {
namespace fs = std::filesystem;
fs::path U8p(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string P8s(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}
const char* Env(const char* n) {
    const char* v = std::getenv(n);
    return v && *v ? v : nullptr;
}

struct Player {
    std::shared_ptr<ThpSession> s;
    std::string key;               // path + still of the current film
    bool have_draft = false;
    thp::BackgroundPrefs draft;
    bool failed = false;           // this film is unavailable for the rest of the session
    bool tex_has_video = false;
    bool have_resume = false;
    uint64_t resume_k = 0;
    thp::MediaClock clock;
    bool clock_started = false;
    bool music_on = false;         // what the session was told
    bool audio_dead = false;       // no device / stalled: wall clock for the rest of the process
    bool device_checked = false;
    float focus_gain = 1.0f, last_gain = -1.0f;
    double in_abs = 0, out_abs = 0; // pushed music this stats period: |source| and |after gain| (the measured gain)
    std::string refused_file;       // background_video_file refused once (logged once)
    double last_call = -1;
    GLuint size_tex = 0;           // texture whose size is known
    int tex_w = 0, tex_h = 0;
    double next_stats = 0;
    std::vector<int16_t> blk;
};
void OnProcessExit();
Player& P() {
    // Never freed: no static destructor ever races the decode thread. std::atexit runs before the CRT tears stdio down,
    // so the thread is stopped and joined there (Quit from the launcher) instead of being cut off mid-line by ExitProcess.
    static Player* p = [] {
        Player* q = new Player;
        std::atexit(OnProcessExit);
        return q;
    }();
    return *p;
}
void OnProcessExit() {
    if (std::shared_ptr<ThpSession> s = P().s) {
        s->RequestStop();
        s->WaitStopped(1.0);
    }
}

int LatencyMs() {
    const char* l = Env("NWII_AUDIO_LATENCY_MS");
    return l ? std::clamp(std::atoi(l), 20, 1000) : 80; // = platform/audio_out's target
}

double StatsPeriod() {
    const char* v = Env("NWII_UI_VIDEO_STATS");
    if (!v || v[0] == '0') return 0;
    const int n = std::atoi(v);
    return n >= 2 ? std::min(n, 600) : 10.0;
}

// Uploads w x h pixels (fmt GL_BGRA or GL_RGBA) into tex (created when 0). Restores the bindings it touches.
bool UploadTex(Player& p, unsigned int& tex, const uint8_t* px, int w, int h, GLenum fmt) {
    for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i) {}
    GLint prev_tex = 0, prev_pbo = 0, prev_align = 4, prev_row = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &prev_pbo);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_align);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &prev_row);
    if (prev_pbo) glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    if (!tex) {
        GLuint t = 0;
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        tex = t;
        p.size_tex = t;
        p.tex_w = p.tex_h = 0;
    }
    glBindTexture(GL_TEXTURE_2D, tex);
    if (p.size_tex != tex) {
        GLint tw = 0, th = 0;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
        p.size_tex = tex;
        p.tex_w = tw;
        p.tex_h = th;
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    if (p.tex_w != w || p.tex_h != h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, fmt, GL_UNSIGNED_BYTE, px);
        p.tex_w = w;
        p.tex_h = h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, fmt, GL_UNSIGNED_BYTE, px);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, prev_align);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, prev_row);
    glBindTexture(GL_TEXTURE_2D, GLuint(prev_tex));
    if (prev_pbo) glBindBuffer(GL_PIXEL_UNPACK_BUFFER, GLuint(prev_pbo));
    return glGetError() == GL_NO_ERROR;
}

// Video off (or failed) after video frames were shown: the still frame N again, as UI13's LoadLook decoded it.
void RestoreStill(Player& p, const std::string& path, int still, unsigned int& tex, int& w, int& h) {
    std::vector<uint8_t> jpeg;
    Image im;
    std::string err;
    if (ThpFrameJpeg(path, still, jpeg, nullptr, &err) && DecodeImageWic(jpeg, im, &err) &&
        UploadTex(p, tex, im.rgba.data(), im.w, im.h, GL_RGBA)) {
        w = thp::DisplayWidth(im.w, im.h);
        h = im.h;
    } else {
        Log("still frame unavailable (" + err + ")");
    }
}

void StopSession(Player& p, bool keep_resume) {
    if (!p.s) return;
    if (keep_resume) {
        p.have_resume = true;
        p.resume_k = p.s->any_shown() ? p.s->shown_k() + 1 : p.s->params().k0;
    }
    p.s->RequestStop();
    p.s.reset();
    p.music_on = false;
    p.clock_started = false;
}
} // namespace

void ThpBackgroundPreview(const ThpBackgroundPrefs* draft) {
    Player& p = P();
    p.have_draft = draft != nullptr;
    if (draft) p.draft = *draft;
}

void ThpBackgroundShutdown() {
    Player& p = P();
    if (!p.s) return;
    std::shared_ptr<ThpSession> s = p.s;
    StopSession(p, true);
    s->WaitStopped(1.0);
}

ThpBackgroundStats ThpBackgroundGetStats() {
    Player& p = P();
    ThpBackgroundStats r;
    r.failed = p.failed;
    r.t = p.clock.t();
    r.audio_clock = p.clock.audio();
    if (p.s) {
        const ThpSession::State st = p.s->state();
        r.active = st == ThpSession::State::Starting || st == ThpSession::State::Playing;
        r.playing = st == ThpSession::State::Playing && p.s->any_shown();
        const ThpSession::Stats ss = p.s->stats();
        r.shown = ss.shown;
        r.decoded = ss.decoded;
        r.dropped = ss.dropped;
        r.errors = ss.errors;
        r.decode_ms = ss.decode_ms_avg;
        r.k = p.s->shown_k();
    }
    return r;
}

bool ThpBackgroundFrame(const std::string& game_dir, unsigned int& tex, int& w, int& h) {
    Player& p = P();
    const double now = ThpNow();
    const double gap = p.last_call < 0 ? 0.0 : now - p.last_call;
    p.last_call = now;
    const auto fix_aspect = [&] {
        if (w > 0 && h > 0) w = thp::DisplayWidth(w, h);
    };

    const runtime::UiConfig& u = runtime::ui_config();
    thp::PrefsInputs in;
    in.config.video = u.background_video;
    in.config.music = u.background_music;
    in.config.volume = u.background_music_volume;
    in.config.file = u.background_video_file;
    in.config.spec = u.background;
    in.draft = p.have_draft ? &p.draft : nullptr;
    in.env_video = Env("NWII_UI_VIDEO");
    in.env_music = Env("NWII_UI_MUSIC");
    in.env_volume = Env("NWII_UI_MUSIC_VOLUME") ? Env("NWII_UI_MUSIC_VOLUME") : Env("NWII_UI_VIDEO_VOLUME");
    in.env_file = Env("NWII_UI_VIDEO_FILE");
    in.env_background = Env("NWII_UI_BACKGROUND");
    in.ui_test = Env("NWII_UI_AUTOTEST") || Env("NWII_UI_SELFTEST");
    in.audio_dump = Env("NWII_AUDIO_DUMP") != nullptr;
    const thp::BackgroundPrefs pr = thp::ResolvePrefs(in);
    std::string still_file, file;
    int still = 0, start = 0;
    thp::ParseBackgroundSpec(pr.spec, still_file, still); // the still ([ui] background), shown while the video is off
    if (!thp::ResolveFilm(pr, file, start) && pr.file != p.refused_file) {
        p.refused_file = pr.file;
        Log("background_video_file \"" + pr.file + "\" is not a .thp file name in files/THP: using " + file);
    }
    const std::string still_path = P8s(U8p(game_dir) / "files" / "THP" / U8p(still_file));
    const std::string path = P8s(U8p(game_dir) / "files" / "THP" / U8p(file));

    if (!pr.video) {
        StopSession(p, true);
        if (p.tex_has_video) {
            RestoreStill(p, still_path, still, tex, w, h);
            p.tex_has_video = false;
        }
        fix_aspect();
        return false;
    }
    const std::string key = path + ":" + std::to_string(start);
    if (key != p.key) { // another film (or the first call): start over, the still until the new film's first frame
        StopSession(p, false);
        p.key = key;
        p.failed = false;
        p.have_resume = false;
        if (p.tex_has_video) {
            RestoreStill(p, still_path, still, tex, w, h);
            p.tex_has_video = false;
        }
    }
    if (p.failed) {
        fix_aspect();
        return false;
    }
    if (p.s && p.s->state() == ThpSession::State::Stopped) StopSession(p, true); // idle stop: resume where it was
    if (!p.s) {
        ThpSessionParams sp;
        sp.path = path;
        sp.file = file;
        sp.still = start;
        sp.k0 = p.have_resume ? p.resume_k : 0;
        sp.music = pr.MusicOn() && !p.audio_dead;
        sp.audio_fifo_ms = uint32_t(LatencyMs() + 400);
        p.s = ThpSession::Start(sp);
        p.music_on = sp.music;
        p.clock_started = false;
        fix_aspect();
        return false;
    }
    if (gap > 1.0 && StatsPeriod() > 0)
        Log(Fmt("launcher frames again after %.1f s (k=%llu)", gap, (unsigned long long)p.s->shown_k()));
    ThpSession& s = *p.s;
    s.Touch(now);
    const ThpSession::State st = s.state();
    if (st == ThpSession::State::Failed) {
        p.failed = true;
        StopSession(p, false);
        fix_aspect();
        return false;
    }
    if (st != ThpSession::State::Playing) {
        fix_aspect();
        return false;
    }
    const thp::MovieInfo& mi = s.info();
    const double fps = mi.fps;
    const uint64_t k0 = s.params().k0;
    if (!p.clock_started) {
        p.clock.Reset(double(k0) / fps);
        if (p.music_on && mi.audio_channels) {
            p.clock.StartAudio(double(k0) / fps);
            Log(Fmt("music on: volume %d %% (gain %.3f)", pr.volume, double(pr.Gain())));
        }
        p.clock_started = true;
        p.device_checked = false;
        p.last_gain = -1.0f;
    }
    // music on/off (live: settings, preview, no device)
    const bool want_music = pr.MusicOn() && mi.audio_channels > 0 && !p.audio_dead;
    if (want_music != p.music_on) {
        const uint64_t seek = s.any_shown() ? s.shown_k() + 1 : k0;
        s.SetMusic(want_music, seek);
        p.music_on = want_music;
        p.last_gain = -1.0f;
        if (want_music) p.clock.StartAudio(double(seek) / fps);
        Log(want_music ? Fmt("music on: volume %d %% (gain %.3f)", pr.volume, double(pr.Gain()))
                       : std::string("music off"));
    }
    // focus: the music is muted (0.3 s fade) while the launcher window is not active (the picture keeps playing)
    SDL_Window* win = SDL_GL_GetCurrentWindow();
    const bool focused = win && (SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS) != 0;
    p.focus_gain = thp::StepFocusGain(p.focus_gain, focused, gap);
    // audio out + clock
    if (p.music_on) {
        const runtime::audio::OutStats st0 = runtime::audio::stats();
        const uint32_t rate = mi.audio_rate;
        const int lat_ms = LatencyMs();
        const size_t lat = size_t(uint64_t(rate) * lat_ms / 1000), target = size_t(uint64_t(rate) * (lat_ms + 25) / 1000);
        size_t fill = st0.fill;
        const float g1 = pr.Gain() * p.focus_gain;
        bool pushed = false;
        while (fill < target && s.PopAudio(p.blk)) {
            const size_t n = p.blk.size() / 2;
            if (n == 0) continue;
            thp::ApplyGainRamp(p.blk.data(), n, p.last_gain < 0 ? g1 : p.last_gain, g1, &p.in_abs, &p.out_abs);
            runtime::audio::push_samples(p.blk.data(), n, rate);
            p.last_gain = g1;
            fill += n;
            pushed = true;
        }
        if (pushed && !p.device_checked) {
            p.device_checked = true;
            if (!runtime::audio::stats().device_open) {
                p.audio_dead = true;
                Log("no audio device: the video runs on the wall clock, without music");
            }
        }
        if (!p.audio_dead) {
            p.clock.TickAudio(runtime::audio::stats().played, rate, now, st0.fill >= lat);
            if (p.clock.AudioStalled(now)) {
                p.audio_dead = true;
                Log("audio device stalled: the video runs on the wall clock, without music");
            }
        }
        if (p.audio_dead) {
            s.SetMusic(false, 0);
            p.music_on = false;
            p.clock.TickWall(now);
        }
    } else {
        p.clock.TickWall(now);
    }
    const double t = p.clock.t();
    s.PublishClock(t);
    // video
    bool gl_ok = true;
    const bool took = s.TakeFrame(t, [&](const uint8_t* px, int fw, int fh, uint64_t) {
        gl_ok = UploadTex(p, tex, px, fw, fh, GL_BGRA);
        return gl_ok;
    });
    if (!gl_ok) {
        Log("GL upload failed: still frame");
        p.failed = true;
        StopSession(p, false);
        fix_aspect();
        return false;
    }
    if (took) p.tex_has_video = true;
    if (p.tex_has_video) {
        w = thp::DisplayWidth(int(mi.width), int(mi.height));
        h = int(mi.height);
    } else {
        fix_aspect();
    }
    if (const double per = StatsPeriod(); per > 0 && now >= p.next_stats) {
        if (p.next_stats > 0) {
            const ThpSession::Stats ss = s.stats();
            Log(Fmt("shown=%llu decoded=%llu dropped=%llu errors=%llu decode_ms=%.2f clock=%s t=%.2f k=%llu frame=%u "
                    "fill=%u music=%d vol=%d focus=%.2f gain=%.3f measured=%.3f",
                    (unsigned long long)ss.shown, (unsigned long long)ss.decoded, (unsigned long long)ss.dropped,
                    (unsigned long long)ss.errors, ss.decode_ms_avg, p.clock.audio() ? "audio" : "wall", t,
                    (unsigned long long)s.shown_k(), s.plan().FrameAt(s.shown_k()),
                    unsigned(runtime::audio::stats().fill), int(p.music_on), pr.volume, double(p.focus_gain),
                    double(pr.Gain()), p.in_abs > 0 ? p.out_abs / p.in_abs : 0.0));
        }
        p.in_abs = p.out_abs = 0;
        p.next_stats = now + per;
    }
    return p.tex_has_video;
}
#endif // THPVID19_SESSION_ONLY

} // namespace nwii::ui
