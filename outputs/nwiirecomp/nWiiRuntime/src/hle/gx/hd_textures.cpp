// HDTEX16: Dolphin-compatible custom ("HD") textures + texture dumping (see runtime/gx/hd_textures.h).
#ifdef _WIN32 // before glad (APIENTRY)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#endif
#include "runtime/gx/hd_textures.h"

#include "runtime/config.h"
#include "runtime/mods.h"
#include "runtime/gx/dolphin_texname.h"
#include "runtime/gx/glyph_sets.h" // CTRL18-UI
#include "runtime/gx_state.h"
#include "runtime/hdtex_config.h"
#include "runtime/texture_cache_gl.h"


#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nwii::runtime::hle {

extern uint64_t g_texture_cache_generation; // texture_cache_gl.cpp

namespace {
namespace fs = std::filesystem;
using namespace nwii::runtime::gx::hdtex;
using Clock = std::chrono::steady_clock;

constexpr GLenum kGL_COMPRESSED_RGBA_S3TC_DXT1 = 0x83F1;
constexpr GLenum kGL_COMPRESSED_RGBA_S3TC_DXT5 = 0x83F3;
constexpr GLenum kGL_COMPRESSED_RGBA_BPTC_UNORM = 0x8E8C;

const char* env(const char* n) {
    const char* v = std::getenv(n);
    return (v && *v) ? v : nullptr;
}
int env_int(const char* n, int def) {
    const char* v = env(n);
    return v ? std::atoi(v) : def;
}
std::string u8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}
fs::path from_u8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

fs::path exe_dir() {
#ifdef _WIN32
    std::vector<wchar_t> buf(32768);
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
    if (n > 0 && n < buf.size()) return fs::path(std::wstring(buf.data(), n)).parent_path();
#endif
    return fs::current_path();
}

// ---- decoded images -----------------------------------------------------------------------------
struct Level {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> data; // RGBA8 rows (top first) or compressed blocks
};
struct Decoded {
    GLenum compressed = 0; // 0 = RGBA8
    std::vector<Level> levels;
    size_t bytes = 0;
};

struct Entry {
    std::string key;
    fs::path path; // level 0 file
    bool arb = false, dds = false;
    uintmax_t file_size = 0;          // directory entry data (alias detection)
    fs::file_time_type file_time{};
    enum : uint8_t { kIdle, kQueued, kDecoding, kReady, kFailed } state = kIdle; // guarded by State::mtx
    std::shared_ptr<const Decoded> decoded;                           // guarded by State::mtx
    uint64_t last_use = 0;                                            // guarded by State::mtx
    mutable uint32_t uploads = 0; // HDTEX17: GL uploads of this entry (GL thread; > 1 = re-upload after a cache clear / alias)
    // HDTEX17 (GL thread only): hook-owned GL texture per mip variant (0 = the decoded levels, 1 = + generated mips),
    // shared by every texture-cache entry of this pack entry and kept across cache clears (see EvictShared).
    mutable GLuint gl[2] = {0, 0};
    mutable uint64_t gl_gen[2] = {0, 0};   // cache generation of the last use
    mutable uint32_t gl_levels[2] = {0, 0};
    mutable size_t gl_bytes[2] = {0, 0};
    mutable float gl_scale = 1.0f;
    mutable uint32_t gl_hybrid[2] = {0, 0}; // CustomTexInfo::hybrid_base of each variant
    mutable int8_t single = -1;             // decoded image is one uncompressed level (mips can be generated): 1/0, -1 unknown
};

struct Pending {
    GLuint tex;
    uint64_t gen;
    int entry;
    uint32_t orig_levels, native_w, native_h;
    uint64_t frame = 0; // HDTEX17: frame of the request (pop-in time = upload frame - this)
    std::vector<Level> orig_mips; // HDTEX17: the game's own mip levels 1.. (hybrid chain, see ApplyOrigMips)
};

struct DumpJob {
    fs::path path;
    uint32_t w, h;
    std::vector<uint8_t> rgba;
};

#ifdef _WIN32
struct ComThread {
    bool uninit = false;
    IWICImagingFactory* f = nullptr;
    ComThread() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        uninit = SUCCEEDED(hr);
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    }
    ~ComThread() {
        if (f) f->Release();
        if (uninit) CoUninitialize();
    }
};
template <class T> struct ComPtr {
    T* p = nullptr;
    ~ComPtr() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
};

bool DecodePngWic(IWICImagingFactory* f, const fs::path& path, Level& out) {
    if (!f) return false;
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(f->CreateDecoderFromFilename(path.wstring().c_str(), nullptr, GENERIC_READ,
                                            WICDecodeMetadataCacheOnDemand, &dec)))
        return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame))) return false;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(f->CreateFormatConverter(&conv))) return false;
    // 32bppRGBA = straight (non-premultiplied) alpha; no colour management: values as stored.
    if (FAILED(conv->Initialize(frame.p, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                WICBitmapPaletteTypeCustom)))
        return false;
    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);
    if (!w || !h || w > 16384 || h > 16384) return false;
    out.w = w;
    out.h = h;
    out.data.resize(size_t(w) * h * 4);
    return SUCCEEDED(conv->CopyPixels(nullptr, w * 4, UINT(out.data.size()), out.data.data()));
}

bool EncodePngWic(IWICImagingFactory* f, const fs::path& path, uint32_t w, uint32_t h, std::vector<uint8_t>& px) {
    if (!f) return false;
    ComPtr<IWICStream> stream;
    if (FAILED(f->CreateStream(&stream))) return false;
    if (FAILED(stream->InitializeFromFilename(path.wstring().c_str(), GENERIC_WRITE))) return false;
    ComPtr<IWICBitmapEncoder> enc;
    if (FAILED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc))) return false;
    if (FAILED(enc->Initialize(stream.p, WICBitmapEncoderNoCache))) return false;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(enc->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->SetSize(w, h)))
        return false;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppRGBA;
    if (FAILED(frame->SetPixelFormat(&fmt))) return false;
    if (fmt != GUID_WICPixelFormat32bppRGBA) // encoder picked BGRA
        for (size_t i = 0; i + 3 < px.size(); i += 4) std::swap(px[i], px[i + 2]);
    if (FAILED(frame->WritePixels(h, w * 4, UINT(px.size()), px.data())) || FAILED(frame->Commit()) ||
        FAILED(enc->Commit()))
        return false;
    return true;
}
#endif

// ---- DDS (optional): RGBA8 / BGRA8, BC1, BC3, BC7 ------------------------------------------------------
uint32_t rd32le(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

bool DecodeDds(const fs::path& path, Decoded& out, bool have_s3tc, bool have_bptc, std::string* why) {
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> f((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (f.size() < 128 || std::memcmp(f.data(), "DDS ", 4) != 0) { *why = "no DDS header"; return false; }
    const uint8_t* h = f.data() + 4;
    const uint32_t height = rd32le(h + 8), width = rd32le(h + 12);
    uint32_t mips = rd32le(h + 24);
    const uint8_t* pf = h + 72;
    const uint32_t pf_flags = rd32le(pf + 4), fourcc = rd32le(pf + 8), bits = rd32le(pf + 12);
    const uint32_t rmask = rd32le(pf + 16), amask = rd32le(pf + 28);
    size_t off = 128;
    GLenum fmt = 0;
    uint32_t block = 0;
    bool bgra = false;
    auto cc = [](const char* s) { return uint32_t(uint8_t(s[0])) | uint32_t(uint8_t(s[1])) << 8 | uint32_t(uint8_t(s[2])) << 16 | uint32_t(uint8_t(s[3])) << 24; };
    if ((pf_flags & 4) && fourcc == cc("DX10")) {
        if (f.size() < 148) { *why = "short DX10 header"; return false; }
        const uint32_t dxgi = rd32le(f.data() + 128);
        off = 148;
        switch (dxgi) {
        case 71: case 72: fmt = kGL_COMPRESSED_RGBA_S3TC_DXT1; block = 8; break;
        case 77: case 78: fmt = kGL_COMPRESSED_RGBA_S3TC_DXT5; block = 16; break;
        case 98: case 99: fmt = kGL_COMPRESSED_RGBA_BPTC_UNORM; block = 16; break;
        case 28: case 29: break;             // R8G8B8A8
        case 87: case 91: bgra = true; break; // B8G8R8A8
        default: *why = "DXGI format " + std::to_string(dxgi) + " not supported"; return false;
        }
    } else if (pf_flags & 4) {
        if (fourcc == cc("DXT1")) { fmt = kGL_COMPRESSED_RGBA_S3TC_DXT1; block = 8; }
        else if (fourcc == cc("DXT5")) { fmt = kGL_COMPRESSED_RGBA_S3TC_DXT5; block = 16; }
        else { *why = "FourCC not supported (DXT1, DXT5, DX10 BC1/BC3/BC7/RGBA8)"; return false; }
    } else if ((pf_flags & 0x40) && bits == 32 && amask == 0xFF000000u) {
        bgra = rmask == 0x00FF0000u;
    } else {
        *why = "pixel format not supported";
        return false;
    }
    if (fmt == kGL_COMPRESSED_RGBA_BPTC_UNORM && !have_bptc) { *why = "BC7 not supported by this GL driver"; return false; }
    if ((fmt == kGL_COMPRESSED_RGBA_S3TC_DXT1 || fmt == kGL_COMPRESSED_RGBA_S3TC_DXT5) && !have_s3tc) {
        *why = "S3TC not supported by this GL driver";
        return false;
    }
    if (!width || !height || width > 16384 || height > 16384) { *why = "bad size"; return false; }
    if (mips == 0) mips = 1;
    out.compressed = fmt;
    uint32_t w = width, hh = height;
    for (uint32_t l = 0; l < mips && l < 16; ++l) {
        const size_t sz = fmt ? size_t(std::max(1u, (w + 3) / 4)) * std::max(1u, (hh + 3) / 4) * block : size_t(w) * hh * 4;
        if (off + sz > f.size()) {
            if (l == 0) { *why = "truncated"; return false; }
            break;
        }
        Level lv;
        lv.w = w;
        lv.h = hh;
        lv.data.assign(f.begin() + off, f.begin() + off + sz);
        if (bgra)
            for (size_t i = 0; i + 3 < lv.data.size(); i += 4) std::swap(lv.data[i], lv.data[i + 2]);
        out.levels.push_back(std::move(lv));
        off += sz;
        w = std::max(1u, w / 2);
        hh = std::max(1u, hh / 2);
    }
    return true;
}

// ---- state ----------------------------------------------------------------------------------------
struct State {
    // settings
    bool active = false, dump = false, preload = false, genmips = true, strict = false, sync = false, log_each = false;
    bool cmpr_alpha0 = true; // HDTEX17: NWII_HDTEX_CMPR_ALPHA0 (see CmprTransparentBlack)
    size_t ram_cap = size_t(1536) << 20, upload_budget = size_t(32) << 20;
    double upload_ms_budget = 3.0; // per frame; one upload always proceeds
    int threads = 2;
    std::string game_id = "SOUE01";
    std::vector<fs::path> pack_dirs;
    fs::path dump_dir;

    // index (read-only after init)
    std::vector<Entry> entries;
    std::unordered_map<std::string, int> index;
    uint64_t pack_files = 0, mip_files = 0, dup_files = 0, dds_files = 0, alias_files = 0;
    uint64_t excluded_files = 0;                 // HDTEX17: pack files skipped by the exclusion list
    std::unordered_set<std::string> exclude;     // HDTEX17: keys without "_m" (KeyNoMip)
    double index_ms = 0;

    // worker side
    std::mutex mtx;
    std::condition_variable cv;
    std::deque<std::pair<int, bool>> queue; // entry, preload job
    int64_t preload_left = 0;               // preload jobs not yet taken (guarded by mtx)
    bool preload_done = false;              // set by the worker that took the last one; logged by frame_begin
    Clock::time_point preload_t0{};
    bool stop = false;
    size_t ram_bytes = 0;
    uint64_t use_clock = 0;
    std::atomic<uint64_t> decoded{0}, decode_failed{0};
    std::atomic<bool> have_s3tc{false}, have_bptc{false}, gl_checked{false};

    // GL thread
    std::vector<Pending> pending;
    size_t frame_upload_bytes = 0;
    uint64_t frames = 0, vram_gen = ~0ull;
    size_t vram_bytes = 0;
    uint64_t hits[kStageCount] = {}, misses = 0, uploads = 0, uploads_immediate = 0, upload_bytes = 0;
    std::unordered_set<std::string> hit_names, miss_names, logged;
    double max_upload_ms_frame = 0, frame_upload_ms = 0, max_single_upload_ms = 0;
    // perf diagnostics (stat line): frames whose uploads took > 8 ms, GL-thread frame intervals > 50 ms
    // (any cause, compare with a no-hit run), the longest interval; worker decode time.
    uint64_t upload_frames_over8 = 0, frame_gaps_over50 = 0;
    uint64_t reuploads = 0, cache_clears = 0; // HDTEX17
    bool share = true;                                                  // HDTEX17: NWII_HDTEX_SHARE (default 1)
    bool origmips = true;                                               // HDTEX17: NWII_HDTEX_ORIGMIPS (default 1)
    uint64_t hybrid_count = 0;                                          // HDTEX17: textures with the game's own mips
    struct Hybrid {                                                     // HDTEX17: on_new_texture -> on_decoded
        int kind = 0;   // 1: apply to `tex` now, 2: store in pending[pend]
        GLuint tex = 0;
        size_t pend = 0;
        int entry = -1;
        bool shared = false;
        int log2k = -1;
        uint32_t hdw = 0, hdh = 0;
    } hyb;
    uint64_t shared_count = 0, shared_evicted = 0, shared_hits = 0;    // HDTEX17
    size_t shared_bytes = 0;                                            // HDTEX17
    uint64_t max_wait_frames = 0, wait_frames_total = 0; // HDTEX17: deferred uploads, request -> upload
    double max_frame_gap_ms = 0;
    Clock::time_point last_frame_t{};
    std::atomic<uint64_t> decode_us_total{0}, decode_us_max{0};
    bool BudgetLeft() const { return frame_upload_bytes < upload_budget && frame_upload_ms < upload_ms_budget; }
    std::string last_stat;
    Clock::time_point last_stat_t = Clock::now();

    // dump
    std::unordered_set<std::string> dumped_names;
    std::string dump_name; // name of the texture whose decoded levels on_decoded receives
    bool arbcheck = false;                       // HDTEX17: NWII_HDTEX_ARBCHECK=1 (diagnostic)
    std::string arb_name;                        // HDTEX17
    std::unordered_set<std::string> arb_checked; // HDTEX17
    std::mutex dmtx;
    std::condition_variable dcv;
    std::deque<DumpJob> dq;
    size_t dq_bytes = 0;
    std::atomic<uint64_t> dumped{0}, dump_dropped{0}, dump_failed{0};
    bool dump_dir_ok = false;

    // CTRL18-UI: controller glyph sets (<game root>/textures/glyphs/<game id>/<set>/): one index per set folder, the
    // active one is searched before `index`. Switched on the GL thread at frame start (texture-cache clear).
    fs::path glyph_root;
    std::vector<std::string> glyph_names;                          // set folders found at init (sorted)
    std::vector<std::unordered_map<std::string, int>> glyph_index; // per set
    int glyph_active = -1;                                         // GL thread; -1 = none (original pictures)
    std::atomic<int> glyph_request{-2};                            // -2 = nothing pending
    std::atomic<int> glyph_active_pub{-1};                         // for HdTexGlyphSet() on other threads
    std::atomic<uint64_t> glyph_hits{0}, glyph_switches{0};
    std::mutex glyph_mtx;                                          // guards glyph_pending_name (before init)
    std::string glyph_pending_name;                                // a request made before HdTexInit
    bool glyph_pending = false;
    bool glyph_indexed = false;
};
State& S() {
    static State* s = new State; // never destroyed: detached workers may still run at exit
    return *s;
}

// ---- decode (worker threads, or the GL thread with NWII_HDTEX_SYNC=1) --------------------------------
#ifdef _WIN32
IWICImagingFactory* ThreadFactory() {
    thread_local ComThread com;
    return com.f;
}
#endif

// HDTEX17: original GX format from a pack key "tex1_<w>x<h>[_m]_<hash>[_<tlut>]_<fmt>" (-1 if unknown).
int OrigFormat(const std::string& key) {
    const size_t u = key.rfind('_');
    if (u == std::string::npos || u + 1 >= key.size()) return -1;
    int v = 0;
    for (size_t i = u + 1; i < key.size(); ++i) {
        if (key[i] < '0' || key[i] > '9') return -1;
        v = v * 10 + (key[i] - '0');
    }
    return v;
}

// HDTEX17: a CMPR (S3TC-like) original decodes every transparent texel as RGBA 0,0,0,0 (3-colour block mode).
// Materials may use the RGB of those texels (Link's earring samples a transparent block of al_face: blue in the game,
// white/pink with the upscaler's inpainted skin colour), and bilinear filtering at cut-out edges blends towards that
// black. So a custom image replacing a CMPR texture gets RGB 0 wherever its alpha is 0, exactly like the original data.
// NWII_HDTEX_CMPR_ALPHA0=0 keeps the pack's RGB.
void CmprTransparentBlack(Decoded& d) {
    for (Level& l : d.levels)
        for (size_t i = 0; i + 3 < l.data.size(); i += 4)
            if (l.data[i + 3] == 0) l.data[i] = l.data[i + 1] = l.data[i + 2] = 0;
}

std::shared_ptr<Decoded> DecodeEntry(const Entry& e, std::string* why) {
    auto d = std::make_shared<Decoded>();
    State& s = S();
    if (e.dds) {
        if (!DecodeDds(e.path, *d, s.have_s3tc, s.have_bptc, why)) return nullptr;
    } else {
#ifdef _WIN32
        Level l0;
        if (!DecodePngWic(ThreadFactory(), e.path, l0)) { *why = "PNG decode failed"; return nullptr; }
        d->levels.push_back(std::move(l0));
        // Dolphin LoadMips: "<stem>_mip<N><ext>" next to the base file, until the first missing one; a
        // level whose size is not max(prev/2, 1) ends the chain (Dolphin drops it with an error).
        for (int n = 1; n < 16; ++n) {
            const fs::path mp = e.path.parent_path() / (e.path.stem().native() + fs::path("_mip" + std::to_string(n)).native() + e.path.extension().native());
            std::error_code ec;
            if (!fs::exists(mp, ec)) break;
            Level lv;
            if (!DecodePngWic(ThreadFactory(), mp, lv)) break;
            const Level& prev = d->levels.back();
            if (lv.w != std::max(1u, prev.w / 2) || lv.h != std::max(1u, prev.h / 2)) {
                std::cout << "[HDTex] " << u8(mp) << ": mip size " << lv.w << "x" << lv.h << " != "
                          << std::max(1u, prev.w / 2) << "x" << std::max(1u, prev.h / 2) << ", mip chain ends here" << std::endl;
                break;
            }
            d->levels.push_back(std::move(lv));
        }
#else
        *why = "PNG needs Windows (WIC)";
        return nullptr;
#endif
    }
    if (s.cmpr_alpha0 && !d->compressed && OrigFormat(e.key) == 14) CmprTransparentBlack(*d); // HDTEX17
    for (const Level& l : d->levels) d->bytes += l.data.size();
    return d;
}

void EvictLocked(State& s) {
    while (s.ram_bytes > s.ram_cap) {
        int victim = -1;
        uint64_t oldest = ~0ull;
        for (size_t i = 0; i < s.entries.size(); ++i) {
            const Entry& e = s.entries[i];
            if (e.state == Entry::kReady && e.decoded && e.last_use < oldest) {
                oldest = e.last_use;
                victim = int(i);
            }
        }
        if (victim < 0) break;
        Entry& e = s.entries[victim];
        s.ram_bytes -= e.decoded->bytes;
        e.decoded.reset();
        e.state = Entry::kIdle;
    }
}

void StoreDecoded(State& s, int id, std::shared_ptr<Decoded> d, const std::string& why) {
    std::lock_guard<std::mutex> lk(s.mtx);
    Entry& e = s.entries[id];
    if (d) {
        e.decoded = std::move(d);
        e.state = Entry::kReady;
        e.last_use = ++s.use_clock;
        s.ram_bytes += e.decoded->bytes;
        ++s.decoded;
        EvictLocked(s);
    } else {
        e.state = Entry::kFailed;
        ++s.decode_failed;
        std::cout << "[HDTex] cannot load " << u8(e.path) << ": " << why << std::endl;
    }
}

void WorkerMain() {
    State& s = S();
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); // never compete with the game's threads
#endif
    for (;;) {
        std::pair<int, bool> job;
        {
            std::unique_lock<std::mutex> lk(s.mtx);
            s.cv.wait(lk, [&] { return s.stop || !s.queue.empty(); });
            if (s.stop) return;
            job = s.queue.front();
            s.queue.pop_front();
            Entry& e = s.entries[job.first];
            if (job.second && s.preload_left > 0 && --s.preload_left == 0) s.preload_done = true;
            if (e.state != Entry::kQueued) continue;
            if (job.second && s.ram_bytes >= s.ram_cap) { // preload stops at the RAM budget
                e.state = Entry::kIdle;
                continue;
            }
            e.state = Entry::kDecoding; // a second job for this entry (demand + preload) is skipped
        }
        std::string why;
        const auto t0 = Clock::now();
        auto d = DecodeEntry(s.entries[job.first], &why);
        const uint64_t us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count());
        s.decode_us_total += us;
        for (uint64_t m = s.decode_us_max.load(); us > m && !s.decode_us_max.compare_exchange_weak(m, us);) {
        }
        StoreDecoded(s, job.first, std::move(d), why);
    }
}

void DumpMain() {
    State& s = S();
    for (;;) {
        DumpJob job;
        {
            std::unique_lock<std::mutex> lk(s.dmtx);
            s.dcv.wait(lk, [&] { return !s.dq.empty(); });
            job = std::move(s.dq.front());
            s.dq.pop_front();
            s.dq_bytes -= job.rgba.size();
        }
        std::error_code ec;
        if (fs::exists(job.path, ec)) continue; // Dolphin: an existing dump is kept
#ifdef _WIN32
        if (EncodePngWic(ThreadFactory(), job.path, job.w, job.h, job.rgba)) ++s.dumped;
        else ++s.dump_failed;
#endif
    }
}

// Ready -> the decoded image; Idle -> queued (or decoded right here with sync). GL thread.
std::shared_ptr<const Decoded> Acquire(State& s, int id) {
    std::shared_ptr<const Decoded> d;
    bool decode_now = false;
    {
        std::lock_guard<std::mutex> lk(s.mtx);
        Entry& e = s.entries[id];
        if (e.state == Entry::kReady) {
            e.last_use = ++s.use_clock;
            return e.decoded;
        }
        if (e.state == Entry::kQueued && !s.sync) { // waiting in the preload queue: decode it next
            s.queue.emplace_front(id, false);
            s.cv.notify_one();
        }
        if (e.state == Entry::kIdle) {
            if (s.sync) {
                e.state = Entry::kQueued;
                decode_now = true;
            } else {
                e.state = Entry::kQueued;
                s.queue.emplace_front(id, false); // demand before preload
                s.cv.notify_one();
            }
        }
    }
    if (decode_now) {
        std::string why;
        auto dd = DecodeEntry(s.entries[id], &why);
        StoreDecoded(s, id, std::move(dd), why);
        std::lock_guard<std::mutex> lk(s.mtx);
        if (s.entries[id].state == Entry::kReady) d = s.entries[id].decoded;
    }
    return d;
}

uint32_t FullChain(uint32_t w, uint32_t h) {
    uint32_t m = std::max(w, h), n = 1;
    while (m > 1) { m >>= 1; ++n; }
    return n;
}

// GL thread: `tex` gets the custom image (all of its levels; generated mips when the original is
// mipmapped and the pack has none). The GL name stays the same, so the cache entry, the renderer's
// memo and the bound state remain valid.
// HDTEX17: shared = `tex` is a hook-owned texture (recorded in the entry, kept across cache clears).
void Upload(State& s, GLuint tex, const Decoded& d, const Entry& e, uint32_t orig_levels, uint32_t native_w,
            bool shared = false) {
    const auto t0 = Clock::now();
    glBindTexture(GL_TEXTURE_2D, tex);
    const uint32_t n = uint32_t(d.levels.size());
    for (uint32_t l = 0; l < n; ++l) {
        const Level& lv = d.levels[l];
        if (d.compressed)
            glCompressedTexImage2D(GL_TEXTURE_2D, GLint(l), d.compressed, GLsizei(lv.w), GLsizei(lv.h), 0,
                                   GLsizei(lv.data.size()), lv.data.data());
        else
            glTexImage2D(GL_TEXTURE_2D, GLint(l), GL_RGBA8, GLsizei(lv.w), GLsizei(lv.h), 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, lv.data.data());
    }
    uint32_t gl_levels = n;
    size_t bytes = d.bytes;
    if (n == 1 && orig_levels > 1 && s.genmips && !d.compressed) {
        gl_levels = FullChain(d.levels[0].w, d.levels[0].h);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, GLint(gl_levels - 1));
        glGenerateMipmap(GL_TEXTURE_2D);
        bytes = bytes * 4 / 3;
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, GLint(gl_levels - 1));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    CustomTexInfo ci;
    ci.levels = gl_levels;
    ci.scale = native_w ? float(d.levels[0].w) / float(native_w) : 1.0f;
    ci.arb = e.arb;
    TextureCache::get().set_custom(tex, ci);
    s.frame_upload_bytes += d.bytes;
    s.upload_bytes += d.bytes;
    if (shared) { // HDTEX17
        const int v = gl_levels != n ? 1 : 0;
        e.single = (n == 1 && !d.compressed) ? 1 : 0;
        e.gl[v] = tex;
        e.gl_gen[v] = g_texture_cache_generation;
        e.gl_levels[v] = gl_levels;
        e.gl_bytes[v] = bytes;
        e.gl_scale = ci.scale;
        s.shared_bytes += bytes;
        ++s.shared_count;
    } else {
        s.vram_bytes += bytes;
    }
    ++s.uploads;
    if (e.uploads++ > 0) ++s.reuploads; // HDTEX17
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    s.frame_upload_ms += ms;
    s.max_single_upload_ms = std::max(s.max_single_upload_ms, ms);
}

// HDTEX17: log2 of the HD scale when the pack image is one uncompressed level of exactly 2^k x the original size
// (k <= 4), else -1: then the HD chain can continue with the game's own mip levels.
int HybridLog2(const Decoded& d, uint32_t native_w, uint32_t native_h) {
    if (d.levels.size() != 1 || d.compressed || !native_w || !native_h) return -1;
    const uint32_t w = d.levels[0].w, h = d.levels[0].h;
    if (w % native_w || h % native_h || w / native_w != h / native_h) return -1;
    const uint32_t k = w / native_w;
    if (!k || (k & (k - 1)) || k > 16) return -1;
    int l = 0;
    while ((1u << l) < k) ++l;
    return l;
}

// HDTEX17: hybrid mip chain. Upload generated the full chain from the HD image; levels 0..log2k (down to the
// ORIGINAL size) stay the HD ones, the levels below become the game's OWN mip levels 1..n-1 (`mips`). Some
// originals have artist-made mips (B100_WaterHighlight_Mip, the cmn_Fall waterfalls, foliage alpha mips: found with
// NWII_HDTEX_ARBCHECK), which generated mips would replace with a plain average at a distance.
// NWII_HDTEX_ORIGMIPS=0 = HDTEX16 (all levels generated from the HD image).
void ApplyOrigMips(State& s, GLuint tex, int entry, bool shared, int log2k, uint32_t hdw, uint32_t hdh,
                   const std::vector<Level>& mips) {
    glBindTexture(GL_TEXTURE_2D, tex);
    uint32_t levels = uint32_t(log2k) + 1;
    for (const Level& m : mips) {
        const uint32_t ew = std::max(1u, hdw >> levels), eh = std::max(1u, hdh >> levels);
        if (m.w != ew || m.h != eh || m.data.size() < size_t(m.w) * m.h * 4) break;
        glTexSubImage2D(GL_TEXTURE_2D, GLint(levels), 0, 0, GLsizei(m.w), GLsizei(m.h), GL_RGBA, GL_UNSIGNED_BYTE,
                        m.data.data());
        ++levels;
    }
    if (levels == uint32_t(log2k) + 1) return; // nothing applied: the generated chain stays
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, GLint(levels - 1));
    if (const CustomTexInfo* ci = TextureCache::get().custom_info(tex)) {
        CustomTexInfo c = *ci;
        c.levels = levels;
        c.hybrid_base = uint32_t(log2k) + 1; // sampler: max LOD = GX max LOD + log2k
        TextureCache::get().set_custom(tex, c);
    }
    if (shared && entry >= 0) {
        const Entry& e = s.entries[entry];
        for (int v = 0; v < 2; ++v)
            if (e.gl[v] == tex) {
                e.gl_levels[v] = levels;
                e.gl_hybrid[v] = uint32_t(log2k) + 1;
            }
    }
    ++s.hybrid_count;
}

// HDTEX17: at a texture-cache clear (generation `gen` starts) delete the hook-owned textures that no cache entry used
// during the whole previous generation (the cache never deletes them; cache entries of older generations are gone).
void EvictShared(State& s, uint64_t gen) {
    for (const Entry& e : s.entries)
        for (int v = 0; v < 2; ++v)
            if (e.gl[v] && e.gl_gen[v] + 1 < gen) {
                glDeleteTextures(1, &e.gl[v]);
                s.shared_bytes -= std::min(s.shared_bytes, e.gl_bytes[v]);
                e.gl[v] = 0;
                --s.shared_count;
                ++s.shared_evicted;
            }
}

void CheckGL(State& s) {
    GLint n = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (GLint i = 0; i < n; ++i) {
        const char* x = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, GLuint(i)));
        if (!x) continue;
        if (!std::strcmp(x, "GL_EXT_texture_compression_s3tc")) s.have_s3tc = true;
        if (!std::strcmp(x, "GL_ARB_texture_compression_bptc")) s.have_bptc = true;
    }
    GLint major = 0, minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    if (major > 4 || (major == 4 && minor >= 2)) s.have_bptc = true;
    s.gl_checked = true;
}

std::string StatLine(State& s) {
    std::ostringstream o;
    o << "[HDTex] pack " << s.entries.size() << " textures";
    for (int i = 0; i < kStageCount; ++i) o << " " << StageName(i) << "=" << s.hits[i];
    o << " miss=" << s.misses << " (names hit " << s.hit_names.size() << " / miss " << s.miss_names.size() << ")"
      << " uploads=" << s.uploads << " (at miss " << s.uploads_immediate << ", re " << s.reuploads << ") cache_clears="
      << s.cache_clears << " pending=" << s.pending.size()
      << " decoded=" << s.decoded.load() << " failed=" << s.decode_failed.load();
    {
        std::lock_guard<std::mutex> lk(s.mtx);
        o << " ram=" << (s.ram_bytes >> 20) << "MB";
    }
    o << " vram=" << ((s.vram_bytes + s.shared_bytes) >> 20) << "MB (shared " << s.shared_count << " / "
      << (s.shared_bytes >> 20) << "MB, reused " << s.shared_hits << ", evicted " << s.shared_evicted << ")" // HDTEX17
      << " game_mips=" << s.hybrid_count // HDTEX17
      << " max_upload_ms/frame=" << s.max_upload_ms_frame
      << " max_upload_ms/texture=" << s.max_single_upload_ms;
    {
        const uint64_t n = s.decoded.load() + s.decode_failed.load();
        o << " upload_frames>8ms=" << s.upload_frames_over8 << " frames=" << s.frames
          << " gaps>50ms=" << s.frame_gaps_over50 << " max_gap_ms=" << s.max_frame_gap_ms
          << " deferred_wait_frames max=" << s.max_wait_frames << " sum=" << s.wait_frames_total // HDTEX17
          << " decode_ms avg=" << (n ? double(s.decode_us_total.load()) / 1000.0 / double(n) : 0.0)
          << " max=" << double(s.decode_us_max.load()) / 1000.0;
    }
    if (s.dump) o << " dumped=" << s.dumped.load() << " dump_dropped=" << s.dump_dropped.load();
    return o.str();
}

class Hook final : public TextureReplaceHook {
public:
    unsigned on_new_texture(const TexLoadInfo& info, GLuint tex, GLuint* shared) override {
        State& s = S();
        if (!info.base || !info.width || !info.height) return 0;
        const NameParts n = CalcName(info.base, info.width, info.height, info.format, info.mip_filter,
                                     gx::g_state.tlutMem, sizeof(gx::g_state.tlutMem), info.tlut_offset);
        unsigned r = 0;
        if (s.dump && s.dump_dir_ok) {
            std::string full = n.Full();
            if (s.dumped_names.insert(full).second) {
                s.dump_name = std::move(full);
                r |= kWantDecoded;
            }
        }
        if (!s.active) return r;
        int stage = kMiss;
        int id = -1;
        if (s.glyph_active >= 0) { // CTRL18-UI: the active glyph set wins over every pack folder
            id = FindInPack(s.glyph_index[size_t(s.glyph_active)], n, s.strict, &stage);
            if (id >= 0) s.glyph_hits.fetch_add(1, std::memory_order_relaxed);
        }
        if (id < 0) id = FindInPack(s.index, n, s.strict, &stage);
        if (id < 0) {
            ++s.misses;
            const std::string full = n.Full();
            if (s.miss_names.insert(full).second && s.log_each) // HDTEX17: + guest address / levels (diagnostics)
                std::cout << "[HDTex] miss " << full << " addr=0x" << std::hex << info.addr << std::dec
                          << " levels=" << info.levels << " frame=" << s.frames << std::endl;
            return r;
        }
        ++s.hits[stage];
        if (s.hit_names.insert(s.entries[id].key).second && s.log_each)
            std::cout << "[HDTex] hit " << StageName(stage) << " " << n.Full() << " -> " << u8(s.entries[id].path)
                      << std::endl;
        // HDTEX17 diagnostic (NWII_HDTEX_ARBCHECK=1): is the ORIGINAL a texture with arbitrary (artist-made) mips?
        // The pack ships only level 0 and the runtime generates the HD mips, which then differ from the game's.
        if (s.arbcheck && info.levels > 1 && s.arb_checked.insert(s.entries[id].key).second) {
            s.arb_name = n.Full();
            r |= kWantDecoded;
        }
        const Entry& e = s.entries[id];
        // HDTEX17: a mipmapped original gets a hybrid chain (ApplyOrigMips): ask the cache for its decoded levels
        const bool want_orig = s.origmips && s.genmips && info.levels > 1;
        s.hyb = {};
        if (s.share && e.single >= 0) { // HDTEX17: already a hook-owned GL texture: no upload, no low-res frame
            const int v = (e.single == 1 && info.levels > 1 && s.genmips) ? 1 : 0;
            if (e.gl[v]) {
                e.gl_gen[v] = g_texture_cache_generation;
                CustomTexInfo ci;
                ci.levels = e.gl_levels[v];
                ci.scale = e.gl_scale;
                ci.arb = e.arb;
                ci.hybrid_base = e.gl_hybrid[v];
                TextureCache::get().set_custom(e.gl[v], ci);
                ++s.shared_hits;
                *shared = e.gl[v];
                return r | kShared;
            }
        }
        auto d = Acquire(s, id);
        if (d && s.BudgetLeft()) {
            GLuint t = tex;
            if (s.share) glGenTextures(1, &t); // HDTEX17: a new hook-owned texture that every later cache entry shares
            Upload(s, t, *d, e, info.levels, info.width, s.share);
            const int lk = HybridLog2(*d, info.width, info.height);
            if (want_orig && lk >= 0) { // HDTEX17: the game's mips arrive in on_decoded (same get_texture call)
                s.hyb.kind = 1;
                s.hyb.tex = t;
                s.hyb.entry = id;
                s.hyb.shared = s.share;
                s.hyb.log2k = lk;
                s.hyb.hdw = d->levels[0].w;
                s.hyb.hdh = d->levels[0].h;
                r |= kWantDecoded;
            }
            ++s.uploads_immediate;
            if (s.share) {
                *shared = t;
                return r | kShared;
            }
            return r | kReplaced;
        }
        s.pending.push_back({tex, g_texture_cache_generation, id, info.levels, info.width, info.height, s.frames, {}});
        if (want_orig) { // HDTEX17: keep the game's mips for the deferred upload
            s.hyb.kind = 2;
            s.hyb.pend = s.pending.size() - 1;
            r |= kWantDecoded;
        }
        return r;
    }

    void on_decoded(const TexLoadInfo& info, const TexDecodedLevel* levels, uint32_t n) override {
        State& s = S();
        if (s.hyb.kind && n > 1) { // HDTEX17: the game's own mip levels 1..n-1 for the hybrid chain
            std::vector<Level> mips;
            for (uint32_t l = 1; l < n; ++l) {
                Level lv;
                lv.w = levels[l].w;
                lv.h = levels[l].h;
                lv.data.assign(levels[l].rgba, levels[l].rgba + size_t(lv.w) * lv.h * 4);
                mips.push_back(std::move(lv));
            }
            if (s.hyb.kind == 1)
                ApplyOrigMips(s, s.hyb.tex, s.hyb.entry, s.hyb.shared, s.hyb.log2k, s.hyb.hdw, s.hyb.hdh, mips);
            else if (s.hyb.pend < s.pending.size())
                s.pending[s.hyb.pend].orig_mips = std::move(mips);
        }
        s.hyb = {};
        if (!s.arb_name.empty() && n > 1) { // HDTEX17: NWII_HDTEX_ARBCHECK
            std::vector<const uint8_t*> lv;
            for (uint32_t l = 0; l < n; ++l) lv.push_back(levels[l].rgba);
            const bool arb14 = HasArbitraryMipmaps(lv, levels[0].w, levels[0].h, 14.0f);
            const bool arb5 = arb14 || HasArbitraryMipmaps(lv, levels[0].w, levels[0].h, 5.0f);
            if (arb5)
                std::cout << "[HDTex] arbitrary mips " << (arb14 ? "(Dolphin threshold 14)" : "(weak, > 5)") << ": "
                          << s.arb_name << " levels=" << n << " -> the generated HD mips differ from the game's" << std::endl;
        }
        s.arb_name.clear();
        if (s.dump_name.empty() || !n) return;
        for (uint32_t l = 0; l < n; ++l) {
            const size_t bytes = size_t(levels[l].w) * levels[l].h * 4;
            std::lock_guard<std::mutex> lk(s.dmtx);
            if (s.dq_bytes + bytes > (size_t(256) << 20)) { // writer behind: drop, never stall the frame
                ++s.dump_dropped;
                if (l == 0) s.dumped_names.erase(s.dump_name); // try again at the next load
                break;
            }
            DumpJob j;
            j.path = s.dump_dir / from_u8(s.dump_name + (l ? "_mip" + std::to_string(l) : std::string()) + ".png");
            j.w = levels[l].w;
            j.h = levels[l].h;
            j.rgba.assign(levels[l].rgba, levels[l].rgba + bytes);
            s.dq_bytes += bytes;
            s.dq.push_back(std::move(j));
            s.dcv.notify_one();
        }
        (void)info;
        s.dump_name.clear();
    }

    void frame_begin() override {
        State& s = S();
        if (!s.gl_checked) CheckGL(s);
        // CTRL18-UI: glyph set switch (HdTexSetGlyphSet): every cached texture is dropped, so the button pictures are
        // looked up again at their next use (hook-owned HD textures survive the clear; the originals re-decode).
        if (const int req = s.glyph_request.exchange(-2); req != -2 && req != s.glyph_active) {
            s.glyph_active = req;
            s.glyph_active_pub.store(req);
            TextureCache::get().clear();
            s.glyph_switches.fetch_add(1);
            std::cout << "[HDTex] glyph set -> " << (req >= 0 ? s.glyph_names[size_t(req)] : std::string("wii (original)"))
                      << " (frame " << s.frames << ")" << std::endl;
        }
        ++s.frames;
        {
            const auto now = Clock::now();
            if (s.frames > 1) {
                const double gap = std::chrono::duration<double, std::milli>(now - s.last_frame_t).count();
                if (gap > 50.0) ++s.frame_gaps_over50;
                s.max_frame_gap_ms = std::max(s.max_frame_gap_ms, gap);
            }
            s.last_frame_t = now;
        }
        if (s.frame_upload_ms > 8.0) ++s.upload_frames_over8;
        s.max_upload_ms_frame = std::max(s.max_upload_ms_frame, s.frame_upload_ms);
        s.frame_upload_ms = 0;
        s.frame_upload_bytes = 0;
        const uint64_t gen = g_texture_cache_generation;
        if (gen != s.vram_gen) { // the cache was cleared: every custom GL texture is gone
            if (s.vram_gen != ~0ull) ++s.cache_clears; // HDTEX17
            s.vram_gen = gen;
            s.vram_bytes = 0;
            if (s.shared_count) EvictShared(s, gen); // HDTEX17
        }
        if (!s.pending.empty()) {
            GLint unit = 0, bound = 0;
            glGetIntegerv(GL_ACTIVE_TEXTURE, &unit);
            glActiveTexture(GL_TEXTURE0 + 15);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
            size_t keep = 0;
            for (size_t i = 0; i < s.pending.size(); ++i) {
                const Pending p = s.pending[i];
                if (p.gen != gen) continue; // texture deleted by a cache clear
                std::shared_ptr<const Decoded> d;
                bool failed = false;
                if (s.BudgetLeft()) {
                    std::lock_guard<std::mutex> lk(s.mtx);
                    Entry& e = s.entries[p.entry];
                    if (e.state == Entry::kReady) {
                        d = e.decoded;
                        e.last_use = ++s.use_clock;
                    } else if (e.state == Entry::kFailed) {
                        failed = true;
                    } else if (e.state == Entry::kIdle) { // evicted before it was uploaded
                        e.state = Entry::kQueued;
                        s.queue.emplace_front(p.entry, false);
                        s.cv.notify_one();
                    }
                }
                if (failed) continue;
                if (d) {
                    Upload(s, p.tex, *d, s.entries[p.entry], p.orig_levels, p.native_w);
                    if (!p.orig_mips.empty()) // HDTEX17: hybrid chain with the game's own mips
                        if (const int lk = HybridLog2(*d, p.native_w, p.native_h); lk >= 0)
                            ApplyOrigMips(s, p.tex, p.entry, false, lk, d->levels[0].w, d->levels[0].h, p.orig_mips);
                    s.max_wait_frames = std::max(s.max_wait_frames, s.frames - p.frame); // HDTEX17
                    s.wait_frames_total += s.frames - p.frame;
                    continue;
                }
                s.pending[keep++] = p;
            }
            s.pending.resize(keep);
            glBindTexture(GL_TEXTURE_2D, GLuint(bound));
            glActiveTexture(GLenum(unit));
        }
        {
            bool done = false;
            size_t ram = 0;
            {
                std::lock_guard<std::mutex> lk(s.mtx);
                done = s.preload_done;
                s.preload_done = false;
                ram = s.ram_bytes;
            }
            if (done)
                std::cout << "[HDTex] preload: last job taken after "
                          << std::chrono::duration<double>(Clock::now() - s.preload_t0).count() << " s, decoded "
                          << s.decoded.load() << ", ram=" << (ram >> 20) << "MB (cache " << (s.ram_cap >> 20) << "MB)"
                          << std::endl;
        }
        if (Clock::now() - s.last_stat_t > std::chrono::seconds(5)) {
            s.last_stat_t = Clock::now();
            std::string line = StatLine(s);
            if (line != s.last_stat) {
                s.last_stat = line;
                std::cout << line << std::endl;
            }
        }
    }
};

Hook& TheHook() {
    static Hook* h = new Hook;
    return *h;
}

std::string ReadGameId() {
    const std::string& gd = Config::get().game_dir;
    if (!gd.empty()) {
        std::ifstream f(from_u8(gd) / "sys" / "boot.bin", std::ios::binary);
        char id[6] = {};
        if (f.read(id, 6)) {
            bool ok = true;
            for (char c : id) ok = ok && ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'));
            if (ok) return std::string(id, 6);
        }
    }
    return "SOUE01";
}

// "tex1_<w>x<h>[_m]_..." -> the same key with the "_m" flag toggled ("" when the key has no size part).
std::string KeyMipToggled(const std::string& key) {
    if (key.compare(0, 5, "tex1_") != 0) return {};
    size_t p = 5;
    while (p < key.size() && key[p] >= '0' && key[p] <= '9') ++p;
    if (p == 5 || p >= key.size() || key[p] != 'x') return {};
    ++p;
    const size_t h0 = p;
    while (p < key.size() && key[p] >= '0' && key[p] <= '9') ++p;
    if (p == h0) return {};
    if (key.compare(p, 3, "_m_") == 0) return key.substr(0, p) + key.substr(p + 2);
    return key.substr(0, p) + "_m" + key.substr(p);
}

// HDTEX17: "tex1_<w>x<h>_m_..." -> "tex1_<w>x<h>_..." (exclusions match both mip-filter names).
std::string KeyNoMip(const std::string& key) {
    const std::string t = KeyMipToggled(key);
    return (!t.empty() && t.size() < key.size()) ? t : key;
}

// HDTEX17: original SOUE01 textures that are never replaced (game policy like Dolphin's GameSettings, independent of
// the pack: the names are hashes of the ORIGINAL data). Each one was found with gxreplay HD off/on sheets + group
// testing (work/claude-notes/HDTEX17.md, outputs/pc-testresultaten/hdtex17/hd-artefacten.json): an upscaled soft mask /
// glow / particle / atlas changes shape or shows a seam. NWII_HDTEX_EXCLUDE=none disables the list,
// NWII_HDTEX_EXCLUDE=<name,name,...> replaces it.
const char* const kDefaultExclude[] = {
    "tex1_64x64_3b2584b429f307d1_0",    // th_wipe_00_0: fall vignette, soft edge -> hard ring
    "tex1_12x12_e5fcdb60eae7c742_0",    // tr_kirakira_00_0: HUD glow behind hearts/rupees -> star burst
    "tex1_32x32_406f2a969ab7900e_1",    // WK2_Mura (particles): water ripples, soft blobs -> thin lines
    "tex1_128x128_5d9e01ac801bc031_14", // F001r_cmn_tm_carpet atlas: academy floor emblem rays
    "tex1_64x128_195c3067dbfb31bb_14",  // F001r_cmn_po_stend01: seam line on the stained-glass window sill
};

void IndexDir(State& s, const fs::path& dir, std::unordered_map<std::string, int>* into = nullptr) { // CTRL18-UI: into
    std::unordered_map<std::string, int>& index = into ? *into : s.index; // CTRL18-UI: a glyph set has its own index
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    struct File {
        std::string name;
        fs::path path;
        uintmax_t size;
        fs::file_time_type time;
    };
    std::vector<File> files;
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::error_code e2;
        // size / write time come from the directory listing (cached by the iterator on Windows: no file opens)
        const uintmax_t size = it->file_size(e2);
        const fs::file_time_type time = it->last_write_time(e2);
        files.push_back({u8(it->path().filename()), it->path(), size, time});
    }
    std::sort(files.begin(), files.end(), [](const File& a, const File& b) { return a.path < b.path; });
    for (const File& f : files) {
        const std::string& name = f.name;
        const fs::path& path = f.path;
        const PackFileName pf = ParsePackFileName(name);
        if (!pf.ok) continue;
        ++s.pack_files;
        if (pf.dds) ++s.dds_files;
        if (pf.mip > 0) { // loaded next to its base level
            ++s.mip_files;
            continue;
        }
        if (!into && !s.exclude.empty() && s.exclude.count(KeyNoMip(pf.key))) { // HDTEX17: never replaced (CTRL18-UI: !into)
            ++s.excluded_files;
            continue;
        }
        if (index.count(pf.key)) {
            ++s.dup_files;
            continue;
        }
        // Alias: the "_m"-toggled name of an indexed file with the same size and write time is the same image
        // (packs ship both names, e.g. as hard links): share that entry, so it is decoded / preloaded once.
        if (const std::string partner = KeyMipToggled(pf.key); !partner.empty()) {
            const auto it = index.find(partner);
            if (it != index.end()) {
                const Entry& o = s.entries[it->second];
                if (o.file_size == f.size && o.file_time == f.time && o.arb == pf.arb && o.dds == pf.dds && f.size) {
                    index.emplace(pf.key, it->second);
                    ++s.alias_files;
                    continue;
                }
            }
        }
        Entry e;
        e.key = pf.key;
        e.path = path;
        e.arb = pf.arb;
        e.dds = pf.dds;
        e.file_size = f.size;
        e.file_time = f.time;
        index.emplace(pf.key, int(s.entries.size()));
        s.entries.push_back(std::move(e));
    }
}

// CTRL18-UI: index the glyph set folders (one index each) and pick the start set. Skipped for NWII_HD_TEXTURES=0
// (vanilla A/B), for an isolated test pack (NWII_TEXTURE_PACK_DIR without NWII_GLYPH_SET) and when HD textures are off
// and the setting is "wii". The pack counters of the "[HDTex] pack:" line stay those of the pack folders.
void GlyphInit(State& s, const fs::path& root, int want) {
    const char* gs_env = env("NWII_GLYPH_SET");
    const std::string setting = gs_env ? std::string(gs_env) : hdtex_config().glyph_set;
    const char* hd_env = env("NWII_HD_TEXTURES");
    const bool vanilla = hd_env && hd_env[0] == '0';
    const bool isolated = env("NWII_TEXTURE_PACK_DIR") && !gs_env;
    s.glyph_root = env("NWII_GLYPH_DIR") ? from_u8(env("NWII_GLYPH_DIR")) : root / "textures" / "glyphs" / s.game_id;
    const auto t0 = Clock::now();
    size_t files = 0;
    if (!vanilla && !isolated && !(want == 0 && setting == "wii")) {
        std::error_code ec;
        std::vector<std::string> names;
        for (fs::directory_iterator it(s.glyph_root, ec), end; !ec && it != end; it.increment(ec))
            if (it->is_directory(ec)) names.push_back(u8(it->path().filename()));
        std::sort(names.begin(), names.end());
        const uint64_t pf = s.pack_files, mf = s.mip_files, df = s.dup_files, xf = s.dds_files, af = s.alias_files;
        std::vector<std::unordered_map<std::string, int>> idx(names.size());
        for (size_t i = 0; i < names.size(); ++i) IndexDir(s, s.glyph_root / from_u8(names[i]), &idx[i]);
        files = size_t(s.pack_files - pf);
        s.pack_files = pf, s.mip_files = mf, s.dup_files = df, s.dds_files = xf, s.alias_files = af;
        for (size_t i = 0; i < names.size(); ++i)
            if (!idx[i].empty()) s.glyph_names.push_back(names[i]), s.glyph_index.push_back(std::move(idx[i]));
    }
    const auto preset = [](const char* env_name, const std::string& cfg) {
        const char* v = env(env_name);
        return v ? std::string(v) : cfg;
    };
    const std::string pad = preset("NWII_INPUT_PRESET_GAMEPAD", Config::get().preset_gamepad);
    const std::string kb = preset("NWII_INPUT_PRESET_KEYBOARD", Config::get().preset_keyboard);
    std::string folder = glyphs::ResolveFolder(setting, glyphs::Device::Keyboard, pad, kb);
    std::lock_guard<std::mutex> lk(s.glyph_mtx);
    if (s.glyph_pending) folder = s.glyph_pending_name; // the UI already knows the last used device
    int active = -1;
    for (size_t i = 0; i < s.glyph_names.size(); ++i)
        if (s.glyph_names[i] == folder) active = int(i);
    s.glyph_active = active;
    s.glyph_active_pub.store(active);
    s.glyph_indexed = true;
    std::ostringstream o;
    o << "[HDTex] glyph sets: " << s.glyph_names.size() << " (" << files << " files) in " << u8(s.glyph_root)
      << " indexed in " << std::chrono::duration<double, std::milli>(Clock::now() - t0).count() << " ms; setting \""
      << setting << "\" -> " << (active >= 0 ? folder : std::string("wii (original pictures)"));
    if (active < 0 && !folder.empty())
        o << " (set \"" << folder << "\" not found" << (s.glyph_names.empty() ? ", no sets" : "") << ")";
    std::cout << o.str() << std::endl;
}

} // namespace

// CTRL18-UI: glyph set switching (any thread; applied at the next frame start on the GL thread).
void HdTexSetGlyphSet(const std::string& folder) {
    State& s = S();
    std::lock_guard<std::mutex> lk(s.glyph_mtx);
    if (!s.glyph_indexed) {
        s.glyph_pending_name = folder;
        s.glyph_pending = true;
        return;
    }
    int idx = -1;
    for (size_t i = 0; i < s.glyph_names.size(); ++i)
        if (s.glyph_names[i] == folder) idx = int(i);
    if (idx < 0 && !folder.empty() && !s.glyph_names.empty())
        std::cout << "[HDTex] glyph set \"" << folder << "\" not found; original pictures" << std::endl;
    if (idx != s.glyph_active_pub.load()) s.glyph_request.store(idx);
}
std::string HdTexGlyphSet() {
    State& s = S();
    const int a = s.glyph_active_pub.load();
    std::lock_guard<std::mutex> lk(s.glyph_mtx);
    return a >= 0 && size_t(a) < s.glyph_names.size() ? s.glyph_names[size_t(a)] : std::string();
}
std::vector<std::string> HdTexGlyphSets() {
    State& s = S();
    std::lock_guard<std::mutex> lk(s.glyph_mtx);
    return s.glyph_names;
}
uint64_t HdTexGlyphHits() { return S().glyph_hits.load(); }

void HdTexInit() {
    State& s = S();
    const HdTexConfig& c = hdtex_config();
    s.game_id = ReadGameId();
    const fs::path root = exe_dir().parent_path(); // <game root> = the folder above bin/
    const auto flag = [](const char* name, bool def) {
        const char* v = env(name);
        return v ? (v[0] == '1' || v[0] == 't' || v[0] == 'T' || v[0] == 'y') : def;
    };
    s.dump = flag("NWII_TEX_DUMP", c.dump);
    s.preload = flag("NWII_HDTEX_PRELOAD", c.preload);
    s.genmips = flag("NWII_HDTEX_GENMIPS", true);
    s.strict = flag("NWII_HDTEX_STRICT", false);
    s.sync = flag("NWII_HDTEX_SYNC", false);
    s.log_each = flag("NWII_HDTEX_LOG", false);
    s.arbcheck = flag("NWII_HDTEX_ARBCHECK", false); // HDTEX17
    s.share = flag("NWII_HDTEX_SHARE", true);          // HDTEX17
    s.cmpr_alpha0 = flag("NWII_HDTEX_CMPR_ALPHA0", true); // HDTEX17
    s.origmips = flag("NWII_HDTEX_ORIGMIPS", true);    // HDTEX17
    // preload: more decode threads (below-normal priority, see WorkerMain) and a RAM cache of a quarter of the
    // physical memory (at least 1.5 GB, at most 8 GB), so a whole 4x pack fits; env values override both.
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    const int def_threads = s.preload ? int(std::clamp(hw / 4, 2u, 6u)) : 2;
    int def_cache_mb = 1536;
#ifdef _WIN32
    if (s.preload) {
        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof ms;
        if (GlobalMemoryStatusEx(&ms))
            def_cache_mb = int(std::clamp<uint64_t>((ms.ullTotalPhys >> 20) / 4, 1536, 8192));
    }
#endif
    s.threads = std::clamp(env_int("NWII_HDTEX_THREADS", def_threads), 1, 8);
    s.ram_cap = size_t(std::max(64, env_int("NWII_HDTEX_CACHE_MB", def_cache_mb))) << 20;
    s.upload_budget = size_t(std::max(1, env_int("NWII_HDTEX_UPLOAD_MB", 32))) << 20;
    if (const char* v = env("NWII_HDTEX_UPLOAD_MS")) s.upload_ms_budget = std::max(0.1, std::atof(v));
    int want = c.hd_textures;
    if (const char* v = env("NWII_HD_TEXTURES")) want = (v[0] == '1' || v[0] == 't' || v[0] == 'T') ? 1 : 0;

    // Mods have their own enable switch. Turning the built-in HD pack off does
    // not turn off deliberately enabled texture mods.
    const auto mod_dirs = nwii::runtime::mods::TextureDirs();
    for (const auto& dir : mod_dirs) s.pack_dirs.push_back(dir);
    if (want != 0) {
    if (const char* p = env("NWII_TEXTURE_PACK_DIR")) {
        s.pack_dirs.push_back(from_u8(p));
    } else {
        if (!c.pack_dir.empty()) s.pack_dirs.push_back(from_u8(c.pack_dir));
        s.pack_dirs.push_back(root / "textures" / s.game_id);
        s.pack_dirs.push_back(root / "textures" / s.game_id.substr(0, 3)); // Dolphin's region-free folder
        if (!Config::get().config_dir.empty()) {
            const fs::path cd = from_u8(Config::get().config_dir) / "textures" / s.game_id;
            std::error_code ec;
            if (!fs::equivalent(cd, root / "textures" / s.game_id, ec)) s.pack_dirs.push_back(cd);
        }
    }
    }
    { // HDTEX17: exclusion list
        const char* x = env("NWII_HDTEX_EXCLUDE");
        if (!x) {
            if (s.game_id == "SOUE01")
                for (const char* n : kDefaultExclude) s.exclude.insert(n);
        } else if (std::strcmp(x, "none") != 0) {
            std::stringstream ss(x);
            std::string item;
            while (std::getline(ss, item, ','))
                if (!item.empty()) s.exclude.insert(KeyNoMip(item));
        }
    }
    {
        const auto t0 = Clock::now();
        for (size_t i=0;i<s.pack_dirs.size();++i) {
            const size_t first=s.entries.size();
            IndexDir(s,s.pack_dirs[i]);
            if(i<mod_dirs.size()) {
                // Claim both mip-filter variants before the next mod/pack.
                // Otherwise a lower-priority _m file could beat an upper plain file.
                for(size_t j=first;j<s.entries.size();++j) {
                    const auto alias=KeyMipToggled(s.entries[j].key);
                    if(!alias.empty() && s.index.emplace(alias,int(j)).second)++s.alias_files;
                }
            }
        }
        s.index_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    }
    const size_t pack_entries = s.entries.size(); // CTRL18-UI
    GlyphInit(s, root, want);                     // CTRL18-UI: glyph sets (own indexes, own log line)
    s.active = pack_entries > 0 || !s.glyph_names.empty();
    {
        std::ostringstream o;
        o << "[HDTex] pack: " << s.pack_files << " files (" << s.entries.size() << " textures, " << s.mip_files
          << " mip files, " << s.dds_files << " dds, " << s.dup_files << " duplicates, " << s.alias_files
          << " _m aliases, " << s.excluded_files << " excluded) indexed in " << s.index_ms // HDTEX17: excluded
          << " ms from";
        for (const fs::path& d : s.pack_dirs) {
            std::error_code ec;
            if (fs::is_directory(d, ec)) o << " " << u8(d);
        }
        o << "; hd_textures=" << ((pack_entries > 0) ? "on" : (want == 0 ? "off" : "off (no pack)"))
          << (s.preload ? " preload" : "") << (s.genmips ? "" : " no-genmips") << (s.strict ? " strict" : "")
          << " threads=" << s.threads << " cache=" << (s.ram_cap >> 20) << "MB";
        std::cout << o.str() << std::endl;
    }
    if (s.dump) {
        s.dump_dir = env("NWII_TEX_DUMP_DIR") ? from_u8(env("NWII_TEX_DUMP_DIR")) : root / "dump" / "textures" / s.game_id;
        std::error_code ec;
        fs::create_directories(s.dump_dir, ec);
        s.dump_dir_ok = fs::is_directory(s.dump_dir, ec);
        std::cout << "[HDTex] texture dump " << (s.dump_dir_ok ? "on: " : "FAILED (no folder): ") << u8(s.dump_dir)
                  << std::endl;
        if (s.dump_dir_ok) std::thread(DumpMain).detach();
    }
    if (s.active) {
        if (!s.sync)
            for (int i = 0; i < s.threads; ++i) std::thread(WorkerMain).detach();
        if (s.preload) {
            std::lock_guard<std::mutex> lk(s.mtx);
            for (size_t i = 0; i < s.entries.size(); ++i) {
                s.entries[i].state = Entry::kQueued;
                s.queue.emplace_back(int(i), true);
            }
            s.preload_left = int64_t(s.entries.size());
            s.preload_t0 = Clock::now();
            s.cv.notify_all();
        }
    }
    if (s.active || (s.dump && s.dump_dir_ok)) SetTextureReplaceHook(&TheHook());
}

HdTexStats HdTexGetStats() {
    State& s = S();
    HdTexStats r;
    r.active = s.active;
    r.dump = s.dump;
    r.pack_files = s.pack_files;
    r.pack_textures = s.entries.size();
    r.alias_files = s.alias_files;
    for (int i = 0; i < kStageCount; ++i) r.hits[i] = s.hits[i];
    r.misses = s.misses;
    r.uploads = s.uploads;
    r.uploads_immediate = s.uploads_immediate;
    r.upload_bytes = s.upload_bytes;
    r.decoded = s.decoded;
    r.decode_failed = s.decode_failed;
    r.pending = s.pending.size();
    {
        std::lock_guard<std::mutex> lk(s.mtx);
        r.ram_bytes = s.ram_bytes;
    }
    r.vram_bytes = s.vram_bytes + s.shared_bytes; // HDTEX17: + hook-owned (shared) textures
    r.dumped = s.dumped;
    r.dump_dropped = s.dump_dropped;
    r.index_ms = s.index_ms;
    r.max_upload_ms_frame = s.max_upload_ms_frame;
    r.shared_textures = s.shared_count; // HDTEX17
    r.shared_bytes = s.shared_bytes;
    r.shared_evicted = s.shared_evicted;
    r.cache_clears = s.cache_clears;
    r.reuploads = s.reuploads;
    return r;
}

std::string HdTexStatLine() { return StatLine(S()); }

} // namespace nwii::runtime::hle
