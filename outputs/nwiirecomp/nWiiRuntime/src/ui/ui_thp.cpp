// THPVID19: THP reader, DSP-ADPCM decoder, loop plan, media clock (see ui_thp.h). Own code only.
#include "ui_thp.h"

#include "ui_assets.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace nwii::ui::thp {

namespace {
inline uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
inline int16_t be16s(const uint8_t* p) { return int16_t(uint16_t(p[0] << 8 | p[1])); }
inline bool fail(std::string* err, const std::string& m) {
    if (err) *err = m;
    return false;
}
std::FILE* open_utf8(const std::string& path) {
#ifdef _WIN32
    const std::filesystem::path p(std::u8string(path.begin(), path.end()));
    return _wfopen(p.c_str(), L"rb");
#else
    return std::fopen(path.c_str(), "rb");
#endif
}
bool seek(std::FILE* f, uint64_t off) {
#ifdef _WIN32
    return _fseeki64(f, int64_t(off), SEEK_SET) == 0;
#else
    return fseeko(f, off_t(off), SEEK_SET) == 0;
#endif
}
uint64_t file_size(std::FILE* f) {
#ifdef _WIN32
    if (_fseeki64(f, 0, SEEK_END) != 0) return 0;
    const int64_t n = _ftelli64(f);
#else
    if (fseeko(f, 0, SEEK_END) != 0) return 0;
    const int64_t n = int64_t(ftello(f));
#endif
    return n < 0 ? 0 : uint64_t(n);
}
constexpr double kHalfPi = 1.57079632679489661923;
} // namespace

// ------------------------------------------------------------------------------------------------ Movie
void Movie::Close() {
    if (f_) std::fclose(f_);
    f_ = nullptr;
    index_.clear();
    index_.shrink_to_fit();
    info_ = MovieInfo{};
    truncated_ = false;
}

bool Movie::Open(const std::string& path, std::string* err) {
    Close();
    f_ = open_utf8(path);
    if (!f_) return fail(err, "cannot open " + path);
    const auto bad = [&](const std::string& m) {
        Close();
        return fail(err, m);
    };
    MovieInfo& in = info_;
    in.file_size = file_size(f_);
    uint8_t h[48];
    if (!seek(f_, 0) || std::fread(h, 1, 48, f_) != 48 || std::memcmp(h, "THP\0", 4) != 0) return bad("not a THP movie");
    in.version = be32(h + 4);
    if ((in.version >> 16) != 1) return bad("unsupported THP version");
    in.buf_size = be32(h + 8);
    { const uint32_t v = be32(h + 16); std::memcpy(&in.fps, &v, 4); }
    const uint32_t frames = be32(h + 20), first_size = be32(h + 24), comp_off = be32(h + 32), first = be32(h + 40);
    if (!(in.fps > 1.0f && in.fps < 121.0f)) return bad("invalid THP frame rate");
    if (frames == 0 || frames > 1000000) return bad("invalid THP frame count");
    if (uint64_t(comp_off) + 20 > in.file_size || first >= in.file_size) return bad("invalid THP header offsets");
    uint8_t c[20 + 16 * 16];
    std::memset(c, 0, sizeof c);
    if (!seek(f_, comp_off) || std::fread(c, 1, sizeof c, f_) < 20) return bad("THP component table missing");
    const uint32_t ncomp = be32(c);
    if (ncomp == 0 || ncomp > 16) return bad("invalid THP component count");
    const bool v11 = in.version >= 0x00011000;
    const uint8_t* p = c + 20;
    int video_comp = -1, audio_comp = -1;
    for (uint32_t i = 0; i < ncomp; ++i) {
        if (p + 16 > c + sizeof c) return bad("THP component table too long");
        if (c[4 + i] == 0) {
            if (video_comp < 0) { video_comp = int(i); in.width = be32(p); in.height = be32(p + 4); }
            p += v11 ? 12 : 8;
        } else if (c[4 + i] == 1) {
            if (audio_comp < 0) {
                audio_comp = int(i);
                in.audio_channels = be32(p);
                in.audio_rate = be32(p + 4);
                in.audio_samples = be32(p + 8);
            }
            p += v11 ? 16 : 12;
        } else {
            return bad("unknown THP component type");
        }
    }
    if (video_comp < 0) return bad("THP has no video component");
    if (in.width < 16 || in.height < 16 || in.width > 1024 || in.height > 1024) return bad("THP video size out of range");
    if (audio_comp >= 0 && (in.audio_channels < 1 || in.audio_channels > 2 || in.audio_rate < 8000 || in.audio_rate > 48000)) {
        audio_comp = -1; // unusable audio: play the picture silently
        in.audio_channels = in.audio_rate = in.audio_samples = 0;
    }
    const uint32_t hdr = 8 + 4 * ncomp;
    index_.reserve(frames);
    uint64_t pos = first;
    uint32_t size = first_size;
    for (uint32_t i = 0; i < frames; ++i) {
        const bool size_ok = size >= hdr && (in.buf_size == 0 || size <= in.buf_size);
        if (!size_ok || pos + size > in.file_size) {
            if (index_.empty()) return bad(size_ok ? "truncated THP movie" : "invalid THP frame size");
            truncated_ = true; // a damaged or cut-off chain: keep the whole frames before it
            break;
        }
        uint8_t fh[8 + 4 * 16];
        if (!seek(f_, pos) || std::fread(fh, 1, hdr, f_) != hdr) {
            if (index_.empty()) return bad("truncated THP frame");
            truncated_ = true;
            break;
        }
        FrameEntry e;
        e.offset = pos;
        e.size = size;
        uint64_t off = hdr;
        bool inside = true;
        for (uint32_t k = 0; k < ncomp; ++k) {
            const uint32_t cs = be32(fh + 8 + 4 * k);
            if (off + cs > size) { inside = false; break; }
            if (int(k) == video_comp) { e.video_off = uint32_t(off); e.video_size = cs; }
            if (int(k) == audio_comp) { e.audio_off = uint32_t(off); e.audio_size = cs; }
            off += cs;
        }
        if (!inside || e.video_size == 0) {
            if (index_.empty()) return bad("invalid THP component size");
            truncated_ = true;
            break;
        }
        index_.push_back(e);
        pos += size;
        size = be32(fh);
    }
    in.frames = uint32_t(index_.size());
    return true;
}

bool Movie::ReadFrame(uint32_t i, std::vector<uint8_t>& buf) {
    if (!f_ || i >= index_.size()) return false;
    const FrameEntry& e = index_[i];
    buf.resize(e.size);
    return seek(f_, e.offset) && std::fread(buf.data(), 1, e.size, f_) == e.size;
}

bool Movie::ReadAudio(uint32_t i, std::vector<uint8_t>& buf) {
    if (!f_ || i >= index_.size() || index_[i].audio_size == 0) return false;
    const FrameEntry& e = index_[i];
    buf.resize(e.audio_size);
    return seek(f_, e.offset + e.audio_off) && std::fread(buf.data(), 1, e.audio_size, f_) == e.audio_size;
}

bool Movie::ReadVideo(uint32_t i, std::vector<uint8_t>& buf) {
    if (!f_ || i >= index_.size()) return false;
    const FrameEntry& e = index_[i];
    buf.resize(e.video_size);
    return seek(f_, e.offset + e.video_off) && std::fread(buf.data(), 1, e.video_size, f_) == e.video_size;
}

bool VideoJpeg(const uint8_t* raw, size_t n, std::vector<uint8_t>& jpeg, std::string* err) {
    if (!raw || n < 4) return fail(err, "empty THP video frame");
    const std::vector<uint8_t> v(raw, raw + n);
    return RestoreJpegStuffing(v, jpeg, err);
}

// ------------------------------------------------------------------------------------------------ audio
uint32_t detail::DecodeAudioVariant(const uint8_t* a, size_t n, uint32_t nch, std::vector<int16_t>& lr,
                                    int16_t hist_out[2][2], int16_t hist_in[2][2], int variant) {
    // THPAudioRecordHeader is 0x50 bytes for mono and stereo: offsetNextChannel, numSamples, lCoef[8][2], rCoef[8][2],
    // lYn1, lYn2, rYn1, rYn2. Channel ch's data starts at 0x50 + ch * offsetNextChannel.
    if (!a || nch < 1 || nch > 2 || n < 0x50) return 0;
    const uint32_t chsize = be32(a), ns = be32(a + 4);
    if (ns == 0 || ns > 0x100000) return 0;
    const uint64_t need = (uint64_t(ns) + 13) / 14 * 8;
    if (need > chsize || 0x50 + uint64_t(chsize) * nch > n) return 0;
    lr.assign(size_t(ns) * 2, 0);
    for (uint32_t ch = 0; ch < nch; ++ch) {
        const uint8_t* coef = a + 8 + ch * 32;
        int32_t yn1 = be16s(a + 72 + ch * 4), yn2 = be16s(a + 74 + ch * 4);
        if (hist_in) { hist_in[ch][0] = int16_t(yn1); hist_in[ch][1] = int16_t(yn2); }
        const uint8_t* d = a + 0x50 + size_t(ch) * chsize;
        int ps = 0;
        for (uint32_t i = 0; i < ns; ++i) {
            const uint32_t j = i % 14;
            if (j == 0) ps = *d++;
            const int nib = (j & 1) == 0 ? (*d >> 4) : (*d++ & 15);
            const int s4 = nib >= 8 ? nib - 16 : nib;
            const int idx = (ps >> 4) & 7, scale = ps & 15;
            int64_t c1 = be16s(coef + idx * 4), c2 = be16s(coef + idx * 4 + 2);
            if (variant == 2) std::swap(c1, c2);
            int64_t v = ((int64_t(s4) << scale) << 11) + c1 * yn1 + c2 * yn2 + (variant == 1 ? 0 : 0x400);
            v >>= 11;
            v = std::clamp<int64_t>(v, -32768, 32767);
            yn2 = yn1;
            yn1 = int32_t(v);
            lr[size_t(i) * 2 + ch] = int16_t(v);
            if (nch == 1) lr[size_t(i) * 2 + 1] = int16_t(v);
        }
        if (hist_out) { hist_out[ch][0] = int16_t(yn1); hist_out[ch][1] = int16_t(yn2); }
    }
    return ns;
}

uint32_t DecodeAudio(const uint8_t* rec, size_t n, uint32_t channels, std::vector<int16_t>& lr, int16_t hist_out[2][2],
                     int16_t hist_in[2][2]) {
    return detail::DecodeAudioVariant(rec, n, channels, lr, hist_out, hist_in, 0);
}

// ------------------------------------------------------------------------------------------------ loop plan
namespace {
uint32_t eff_xfade(const LoopPlan& p) { return std::min<uint32_t>(p.xfade, (p.loop_out - p.loop_in) / 2); }
} // namespace

uint32_t LoopPlan::FrameAt(uint64_t k) const {
    if (!Valid()) return 0;
    const uint64_t first = loop_out - start;
    if (k < first) return uint32_t(start + k);
    return loop_in + uint32_t((k - first) % (loop_out - loop_in));
}

float LoopPlan::VideoMix(uint64_t k) const {
    if (!Valid()) return 0.0f;
    const uint32_t x = eff_xfade(*this), f = FrameAt(k);
    if (x == 0 || f + x < loop_out) return 0.0f;
    return float(f - (loop_out - x) + 1) / float(x + 1);
}

float LoopPlan::AudioGain(uint64_t k, float pos, uint64_t k_start) const {
    if (!Valid()) return 0.0f;
    pos = std::clamp(pos, 0.0f, 1.0f);
    double g = 1.0;
    const uint32_t x = eff_xfade(*this), f = FrameAt(k);
    if (x && f + x >= loop_out) g *= std::cos(std::min(1.0, (double(f - (loop_out - x)) + pos) / x) * kHalfPi);
    if (afade_in_first && k >= k_start && k - k_start < afade_in_first)
        g *= std::sin((double(k - k_start) + pos) / afade_in_first * kHalfPi);
    const uint64_t first = loop_out - start;
    if (afade_in_wrap && k >= first) {
        const uint64_t j = (k - first) % (loop_out - loop_in);
        if (j < afade_in_wrap) g *= std::sin((double(j) + pos) / afade_in_wrap * kHalfPi);
    }
    return float(std::clamp(g, 0.0, 1.0));
}

LoopPlan DefaultPlan(const std::string& file, const MovieInfo& info, int still_frame) {
    const std::u8string u = std::filesystem::path(std::u8string(file.begin(), file.end())).filename().u8string();
    std::string name(u.begin(), u.end());
    for (char& ch : name) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    LoopPlan p;
    const uint32_t frames = info.frames;
    if (frames == 0) {
        p.start = p.loop_in = 0;
        p.loop_out = 0; // invalid
        return p;
    }
    p.loop_in = 0;
    p.loop_out = frames;
    if (name == "demo01_02.thp" && frames == kDemo0102Frames) {
        p.loop_out = kDemo0102LoopOut; // 2630.. = the emblem/logo on black, then white
    } else if (name == "demo01_01.thp" && frames > kDemo0101Start + 300) {
        p.loop_in = kDemo0101Start; // the first ~12 s are black (the game draws its subtitles there)
    }
    int64_t s = std::max<int64_t>(still_frame, 0);
    if (s < int64_t(p.loop_in) && p.loop_in > 0) s = p.loop_in;
    if (s >= int64_t(p.loop_out)) s = p.loop_in;
    p.start = uint32_t(s);
    if (p.loop_out - p.loop_in < 2) p.xfade = 0;
    return p;
}

void ParseBackgroundSpec(const std::string& spec_in, std::string& file, int& still_frame) {
    const std::string spec = spec_in.empty() ? std::string("Demo01_02.thp:400") : spec_in;
    const size_t colon = spec.rfind(':');
    file = colon == std::string::npos ? spec : spec.substr(0, colon);
    still_frame = colon == std::string::npos ? 0 : std::atoi(spec.c_str() + colon + 1);
}

// ------------------------------------------------------------------------------------------------ clock
void MediaClock::Reset(double t) {
    t_ = t;
    last_now_ = -1;
    audio_ = base_set_ = have_played0_ = false;
    stall_since_ = -1;
}

void MediaClock::StartAudio(double t_first) {
    audio_ = true;
    base_set_ = have_played0_ = false;
    base_t_ = t_first;
    stall_since_ = -1;
}

void MediaClock::TickWall(double now) {
    if (last_now_ >= 0) t_ += std::clamp(now - last_now_, 0.0, 0.1);
    last_now_ = now;
    audio_ = false;
    stall_since_ = -1;
}

void MediaClock::TickAudio(uint64_t played, uint32_t rate, double now, bool expect_progress) {
    const double gap = last_now_ >= 0 ? now - last_now_ : 0.0;
    last_now_ = now;
    if (!audio_) StartAudio(t_);
    if (!have_played0_) {
        have_played0_ = true;
        played0_ = last_played_ = played;
        stall_since_ = -1;
        return;
    }
    if (played > last_played_) {
        base_set_ = true;
        last_played_ = played;
        stall_since_ = -1;
        const double ta = base_t_ + double(played - played0_) / double(rate ? rate : 32000);
        if (ta > t_) t_ = ta; // never backwards
        return;
    }
    if (!expect_progress) stall_since_ = -1;
    else if (stall_since_ < 0 || gap >= 0.1) stall_since_ = now;
}

bool MediaClock::AudioStalled(double now) const { return audio_ && stall_since_ >= 0 && now - stall_since_ >= 0.5; }

int PickSlot(const double* pts, const bool* ready, int n, double t, double after) {
    int best = -1;
    for (int i = 0; i < n; ++i) {
        if (!ready[i] || pts[i] > t || pts[i] <= after) continue;
        if (best < 0 || pts[i] > pts[best]) best = i;
    }
    return best;
}

// ------------------------------------------------------------------------------------------------ prefs
float BackgroundPrefs::Gain() const {
    if (!MusicOn()) return 0.0f;
    const float v = float(std::clamp(volume, 0, 100)) / 100.0f;
    return v * v;
}

BackgroundPrefs ResolvePrefs(const PrefsInputs& in) {
    BackgroundPrefs r = in.draft ? *in.draft : in.config;
    if (in.env_background && *in.env_background) r.spec = in.env_background;
    if (in.env_file && *in.env_file) r.file = in.env_file;
    if (in.env_video && *in.env_video) r.video = in.env_video[0] != '0';
    else if (in.ui_test) r.video = false; // UI test screenshots stay deterministic
    if (in.env_music && *in.env_music) r.music = in.env_music[0] != '0';
    if (in.env_volume && *in.env_volume) {
        char* end = nullptr;
        const long v = std::strtol(in.env_volume, &end, 10);
        if (end != in.env_volume && *end == '\0') r.volume = int(std::clamp<long>(v, 0, 100));
    }
    r.volume = std::clamp(r.volume, 0, 100);
    if (in.audio_dump) r.music = false;
    return r;
}

bool ResolveFilm(const BackgroundPrefs& pr, std::string& file, int& start) {
    std::string still_file;
    int still = 0;
    ParseBackgroundSpec(pr.spec, still_file, still);
    const auto lower = [](std::string s) {
        for (char& ch : s) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        return s;
    };
    bool ok = true;
    if (!pr.file.empty()) {
        std::string name = pr.file;
        int n = -1;
        const size_t colon = name.rfind(':');
        if (colon != std::string::npos && colon + 1 < name.size() &&
            name.find_first_not_of("0123456789", colon + 1) == std::string::npos) {
            n = std::atoi(name.c_str() + colon + 1);
            name.resize(colon);
        }
        const std::string lo = lower(name);
        // REVTHPVID19: files/THP names are printable ASCII. Anything else is refused: an ANSI env value such as
        // "D\xe9mo.thp" is not UTF-8, and std::filesystem throws on it (on the GL thread = a launcher crash).
        const bool ascii = std::all_of(name.begin(), name.end(), [](char ch) { return ch >= 0x20 && ch < 0x7F; });
        const bool plain = ascii && !name.empty() && name.find_first_of("/\\:") == std::string::npos && lo != ".thp" &&
                           lo.size() > 4 && lo.compare(lo.size() - 4, 4, ".thp") == 0;
        if (plain) {
            file = name;
            start = n >= 0 ? n : (lo == lower(still_file) ? still : 0);
            return true;
        }
        ok = false;
    }
    file = still_file;
    start = still;
    return ok;
}

float StepFocusGain(float gain, bool focused, double dt) {
    const float step = float(std::clamp(dt, 0.0, 0.1) / 0.3);
    return focused ? std::min(1.0f, gain + step) : std::max(0.0f, gain - step);
}

void ApplyGainRamp(int16_t* lr, size_t frames, float g0, float g1, double* in_abs, double* out_abs) {
    double si = 0, so = 0;
    for (size_t i = 0; i < frames; ++i) {
        const float g = g0 + (g1 - g0) * float(i + 1) / float(frames);
        for (int c = 0; c < 2; ++c) {
            int16_t& v = lr[2 * i + c];
            si += std::abs(int(v));
            v = int16_t(std::clamp<long>(std::lround(v * g), -32768, 32767));
            so += std::abs(int(v));
        }
    }
    if (in_abs) *in_abs += si;
    if (out_abs) *out_abs += so;
}

int DisplayWidth(int w, int h) {
    (void)h;
    return w == 608 ? int(std::lround(w * 4.0 / 3.0)) : w;
}

} // namespace nwii::ui::thp
