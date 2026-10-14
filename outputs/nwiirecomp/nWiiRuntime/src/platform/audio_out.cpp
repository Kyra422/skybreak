// Host audio output — see audio_out.h for the design and env switches.
#include "audio_out.h"

#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>  // LAUNCH19
#include <vector>

namespace nwii::runtime::audio {

namespace {

const char *env(const char *n) { return std::getenv(n); }

struct Wav {
  FILE *f = nullptr;
  uint32_t rate = 0;
  uint16_t ch = 2;  // SPK11: the speaker dump is mono
  uint64_t frames = 0, last_patch = 0;

  void header() {
    if (!f) return;
    const uint32_t data = (uint32_t)std::min<uint64_t>(frames * 2 * ch, 0xFFFFFFF0ull);
    uint8_t h[44];
    auto w32 = [&](int o, uint32_t v) { h[o] = (uint8_t)v; h[o + 1] = (uint8_t)(v >> 8); h[o + 2] = (uint8_t)(v >> 16); h[o + 3] = (uint8_t)(v >> 24); };
    auto w16 = [&](int o, uint16_t v) { h[o] = (uint8_t)v; h[o + 1] = (uint8_t)(v >> 8); };
    std::memcpy(h, "RIFF", 4); w32(4, 36 + data); std::memcpy(h + 8, "WAVEfmt ", 8);
    w32(16, 16); w16(20, 1); w16(22, ch); w32(24, rate); w32(28, rate * 2 * ch); w16(32, (uint16_t)(2 * ch)); w16(34, 16);
    std::memcpy(h + 36, "data", 4); w32(40, data);
    const long pos = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::fwrite(h, 1, 44, f);
    std::fseek(f, pos < 44 ? 44 : pos, SEEK_SET);
    std::fflush(f);
  }
};

struct Out {
  std::once_flag once;
  bool device_wanted = true;
  SDL_AudioDeviceID dev = 0;
  std::string dev_name;                  // LAUNCH19: [audio] device / NWII_AUDIO_DEVICE ("" = Windows default)
  std::atomic<float> gain_target{1.0f};  // LAUNCH19: master volume x focus mute (set_output_gain)
  float gain_cur = 1.0f;                 // LAUNCH19: callback thread only
  uint32_t dev_rate = 0;
  bool stats_on = false;
  uint32_t latency_ms = 80;

  std::mutex mu;  // guards ring + counters touched by both threads
  std::vector<int16_t> ring;  // interleaved stereo
  size_t cap = 0, rd = 0, count = 0;  // frames
  std::atomic<uint32_t> in_rate{32000};
  bool buffering = true;
  double frac = 0.0, fill_ema = 0.0;
  int16_t last[2] = {0, 0};
  uint32_t fade_left = 0;
  std::vector<int16_t> scratch;

  uint64_t pushed = 0, played = 0, underruns = 0, dropped = 0, next_stats = 0;
  std::atomic<uint64_t> callbacks{0};
  uint64_t t0_ms = 0;
  Wav wav;
  std::mutex wav_mu;

  // SPK11: Wii Remote speaker channels, mixed into the AI stream in push_samples().
  std::mutex spk_mu;  // guards spk + spk_wav
  SpeakerMixer spk[4];
  Wav spk_wav;
  bool spk_wav_tried = false;
};

Out &out() {
  static Out o;
  return o;
}

std::atomic<SpeakerPump> g_speaker_pump{nullptr};  // SPK11 NWII_SPK_SELFTEST

void push_ring_locked(Out &o, const int16_t *lr, size_t frames) {
  if (frames > o.cap) { lr += (frames - o.cap) * 2; o.dropped += frames - o.cap; frames = o.cap; }
  if (o.count + frames > o.cap) {  // overrun: drop oldest
    const size_t drop = o.count + frames - o.cap;
    o.rd = (o.rd + drop) % o.cap;
    o.count -= drop;
    o.dropped += drop;
  }
  size_t wr = (o.rd + o.count) % o.cap;
  for (size_t i = 0; i < frames; ++i) {
    o.ring[2 * wr] = lr[2 * i];
    o.ring[2 * wr + 1] = lr[2 * i + 1];
    if (++wr == o.cap) wr = 0;
  }
  o.count += frames;
}

void SDLCALL callback(void *, Uint8 *stream, int len) {
  Out &o = out();
  int16_t *dst = reinterpret_cast<int16_t *>(stream);
  const size_t want = (size_t)len / 4;
  o.callbacks.fetch_add(1, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(o.mu);
  const double in_rate = o.in_rate.load(std::memory_order_relaxed);
  const size_t target = std::max<size_t>(256, (size_t)(in_rate * o.latency_ms / 1000.0));
  size_t produced = 0;

  if (o.count > target * 4) {  // far too much latency (game faster than real time): drop to target
    const size_t drop = o.count - target;
    o.rd = (o.rd + drop) % o.cap;
    o.count -= drop;
    o.dropped += drop;
  }
  if (o.buffering && o.count >= target) {
    o.buffering = false;
    o.frac = 0.0;
    o.fill_ema = (double)o.count;
  }
  if (!o.buffering) {
    o.fill_ema += ((double)o.count - o.fill_ema) * 0.05;
    const double err = (o.fill_ema - (double)target) / (double)target;
    const double corr = std::clamp(err * 0.01, -0.005, 0.005);
    const double step = in_rate / (double)o.dev_rate * (1.0 + corr);
    const size_t need = std::min(o.count, (size_t)(want * step) + 8);
    o.scratch.resize(need * 2);
    for (size_t i = 0, r = o.rd; i < need; ++i) {
      o.scratch[2 * i] = o.ring[2 * r];
      o.scratch[2 * i + 1] = o.ring[2 * r + 1];
      if (++r == o.cap) r = 0;
    }
    size_t consumed = 0;
    produced = resample_cubic(o.scratch.data(), need, &o.frac, step, dst, want, &consumed);
    o.rd = (o.rd + consumed) % o.cap;
    o.count -= consumed;
    o.played += consumed;
    if (produced) { o.last[0] = dst[2 * produced - 2]; o.last[1] = dst[2 * produced - 1]; }
    if (produced < want) {  // underrun: fade out, then re-buffer to the latency target
      o.underruns++;
      o.buffering = true;
      o.fade_left = 64;
    }
  }
  for (size_t i = produced; i < want; ++i) {
    if (o.fade_left) {
      const int32_t g = (int32_t)--o.fade_left;
      dst[2 * i] = (int16_t)(o.last[0] * g / 64);
      dst[2 * i + 1] = (int16_t)(o.last[1] * g / 64);
    } else {
      dst[2 * i] = dst[2 * i + 1] = 0;
    }
  }
  // LAUNCH19: output gain (master volume, mute while SkyBreak is not active), ramped over one callback (no clicks).
  // Only the device output: the WAV dump keeps the pushed stream.
  const float g1 = std::clamp(o.gain_target.load(std::memory_order_relaxed), 0.0f, 1.0f), g0 = o.gain_cur;
  if (g0 != 1.0f || g1 != 1.0f) {
    for (size_t i = 0; i < want; ++i) {
      const float g = g0 + (g1 - g0) * (float)(i + 1) / (float)want;
      dst[2 * i] = (int16_t)std::lround(dst[2 * i] * g);
      dst[2 * i + 1] = (int16_t)std::lround(dst[2 * i + 1] * g);
    }
  }
  o.gain_cur = g1;
}

// LAUNCH19: opens the host device by name (NWII_AUDIO_DEVICE, set from [audio] device); a missing/unknown device
// falls back to the Windows default. Returns 0 on failure.
SDL_AudioDeviceID open_device(Out &o, SDL_AudioSpec &have) {
  SDL_AudioSpec want{};
  want.freq = 48000;
  want.format = AUDIO_S16SYS;
  want.channels = 2;
  want.samples = 512;
  want.callback = callback;
  const char *name = env("NWII_AUDIO_DEVICE");
  o.dev_name = name ? name : "";
  SDL_AudioDeviceID d = 0;
  if (!o.dev_name.empty()) {
    d = SDL_OpenAudioDevice(o.dev_name.c_str(), 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!d) std::fprintf(stderr, "[AUDIO] device '%s' not available (%s): Windows default\n", o.dev_name.c_str(), SDL_GetError());
  }
  if (!d) d = SDL_OpenAudioDevice(nullptr, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
  if (d && (have.format != AUDIO_S16SYS || have.channels != 2)) {
    SDL_CloseAudioDevice(d);
    d = 0;
  }
  return d;
}

void init_once() {
  Out &o = out();
  const char *a = env("NWII_AUDIO");
  const char *hl = env("NWII_HEADLESS");
  const bool headless = hl && hl[0] == '1';
  o.device_wanted = a ? a[0] != '0' : !headless;
  o.stats_on = env("NWII_AUDIO_STATS") && env("NWII_AUDIO_STATS")[0] == '1';
  if (const char *l = env("NWII_AUDIO_LATENCY_MS")) o.latency_ms = std::clamp(std::atoi(l), 20, 1000);
  o.cap = 65536;
  o.ring.assign(o.cap * 2, 0);
  if (const char *d = env("NWII_AUDIO_DUMP")) {
    o.wav.f = std::fopen(d, "wb");
    if (o.wav.f) {
      o.wav.rate = o.in_rate.load();
      o.wav.header();
      std::fprintf(stderr, "[AUDIO] dumping pushed AI stream to %s\n", d);
    } else {
      std::fprintf(stderr, "[AUDIO] cannot open dump %s\n", d);
    }
  }
  std::atexit([] { shutdown(); });
  if (!o.device_wanted) {
    std::fprintf(stderr, "[AUDIO] host output disabled (NWII_AUDIO=0 or headless)\n");
    return;
  }
  if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
    std::fprintf(stderr, "[AUDIO] SDL audio init failed: %s\n", SDL_GetError());
    return;
  }
  SDL_AudioSpec have{};
  o.dev = open_device(o, have);  // LAUNCH19: by name (NWII_AUDIO_DEVICE)
  if (!o.dev) {
    std::fprintf(stderr, "[AUDIO] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
    return;
  }
  o.dev_rate = (uint32_t)have.freq;
  std::fprintf(stderr, "[AUDIO] host device %d Hz, %d-frame buffer, latency target %u ms\n", have.freq,
               have.samples, o.latency_ms);
  SDL_PauseAudioDevice(o.dev, 0);
}

}  // namespace

size_t resample_cubic(const int16_t *in, size_t in_frames, double *frac, double step, int16_t *out,
                      size_t out_frames, size_t *consumed) {
  size_t base = 0, n = 0;
  double f = *frac;
  while (n < out_frames && base + 3 < in_frames) {
    const int16_t *p = in + base * 2;
    for (int c = 0; c < 2; ++c) {
      const double xm1 = p[c], x0 = p[2 + c], x1 = p[4 + c], x2 = p[6 + c];
      const double y = x0 + 0.5 * f * (x1 - xm1 + f * (2.0 * xm1 - 5.0 * x0 + 4.0 * x1 - x2 +
                                                       f * (3.0 * (x0 - x1) + x2 - xm1)));
      const long v = std::lround(y);
      out[2 * n + c] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    ++n;
    f += step;
    while (f >= 1.0) { f -= 1.0; ++base; }
  }
  *frac = f;
  *consumed = base;
  return n;
}

void push_samples(const int16_t *lr, size_t frames, uint32_t rate) {
  Out &o = out();
  if (!frames || !rate) return;
  o.in_rate.store(rate, std::memory_order_relaxed);
  std::call_once(o.once, init_once);
  if (SpeakerPump pump = g_speaker_pump.load(std::memory_order_relaxed)) pump(frames, rate);  // SPK11 self-test
  {  // SPK11: mix the Wii Remote speaker channels into this AI buffer (both are in virtual time)
    std::lock_guard<std::mutex> lock(o.spk_mu);
    bool any = false;
    for (const SpeakerMixer &m : o.spk) any = any || !m.idle();
    if (any) {
      thread_local std::vector<int16_t> mixed;
      mixed.assign(lr, lr + frames * 2);
      for (SpeakerMixer &m : o.spk) m.mix_into(mixed.data(), frames, rate);
      lr = mixed.data();
    }
  }
  if (o.wav.f) {
    std::lock_guard<std::mutex> lock(o.wav_mu);
    if (o.wav.f) {
      std::vector<uint8_t> le(frames * 4);
      for (size_t i = 0; i < frames * 2; ++i) { le[2 * i] = (uint8_t)lr[i]; le[2 * i + 1] = (uint8_t)((uint16_t)lr[i] >> 8); }
      std::fwrite(le.data(), 1, le.size(), o.wav.f);
      o.wav.frames += frames;
      if (o.wav.frames - o.wav.last_patch >= o.wav.rate) { o.wav.last_patch = o.wav.frames; o.wav.header(); }
    }
  }
  uint64_t pushed;
  uint32_t fill;
  {
    std::lock_guard<std::mutex> lock(o.mu);
    o.pushed += frames;
    if (o.dev) push_ring_locked(o, lr, frames);
    pushed = o.pushed;
    fill = (uint32_t)o.count;
  }
  if (o.stats_on && pushed >= o.next_stats) {
    o.next_stats = pushed + (uint64_t)rate * 10;
    if (!o.t0_ms) o.t0_ms = SDL_GetTicks64();
    std::fprintf(stderr, "[AUDIO] wall=%.1fs pushed=%llu played=%llu fill=%u underruns=%llu dropped=%llu dev=%u cb=%llu st=%d\n",
                 (SDL_GetTicks64() - o.t0_ms) / 1000.0, (unsigned long long)pushed, (unsigned long long)o.played, fill,
                 (unsigned long long)o.underruns, (unsigned long long)o.dropped, o.dev_rate,
                 (unsigned long long)o.callbacks.load(), o.dev ? (int)SDL_GetAudioDeviceStatus(o.dev) : -1);
  }
}

OutStats stats() {
  Out &o = out();
  std::lock_guard<std::mutex> lock(o.mu);
  OutStats s;
  s.pushed = o.pushed; s.played = o.played; s.underruns = o.underruns; s.dropped = o.dropped;
  s.fill = (uint32_t)o.count; s.device_rate = o.dev_rate; s.device_open = o.dev != 0;
  s.latency_ms = o.latency_ms;  // LAUNCH19
  return s;
}

// LAUNCH19
void set_output_gain(float gain) { out().gain_target.store(std::clamp(gain, 0.0f, 1.0f), std::memory_order_relaxed); }
float output_gain() { return out().gain_target.load(std::memory_order_relaxed); }

// LAUNCH19 (fixes REVTHPVID19's finding): the launcher music opens the device before the game, so the latency and
// the device chosen in the launcher were only read at the next start. Called when the game starts.
void apply_host_settings() {
  Out &o = out();
  uint32_t lat = 80;
  if (const char *l = env("NWII_AUDIO_LATENCY_MS")) lat = (uint32_t)std::clamp(std::atoi(l), 20, 1000);
  SDL_AudioDeviceID old = 0;
  bool reopen = false;
  {
    std::lock_guard<std::mutex> lock(o.mu);
    if (lat != o.latency_ms) std::fprintf(stderr, "[AUDIO] latency target %u -> %u ms (LAUNCH19)\n", o.latency_ms, lat);
    o.latency_ms = lat;  // the callback computes its target from this every time
    const char *name = env("NWII_AUDIO_DEVICE");
    reopen = o.dev && o.dev_name != (name ? name : "");
    if (reopen) { old = o.dev; o.dev = 0; }
  }
  if (!reopen) return;
  SDL_CloseAudioDevice(old);  // waits for a running callback (it takes o.mu: never hold it here)
  SDL_AudioSpec have{};
  const SDL_AudioDeviceID d = open_device(o, have);
  {
    std::lock_guard<std::mutex> lock(o.mu);
    o.rd = o.count = 0;
    o.buffering = true;
    if (d) o.dev_rate = (uint32_t)have.freq;
    o.dev = d;
  }
  std::fprintf(stderr, "[AUDIO] host device reopened (%s): %d Hz (LAUNCH19)\n",
               o.dev_name.empty() ? "Windows default" : o.dev_name.c_str(), have.freq);
  if (d) SDL_PauseAudioDevice(d, 0);
}

void shutdown() {
  Out &o = out();
  {
    std::lock_guard<std::mutex> lock(o.wav_mu);
    if (o.wav.f) {
      o.wav.header();
      std::fclose(o.wav.f);
      o.wav.f = nullptr;
    }
  }
  std::lock_guard<std::mutex> lock(o.spk_mu);
  if (o.spk_wav.f) {
    o.spk_wav.header();
    std::fclose(o.spk_wav.f);
    o.spk_wav.f = nullptr;
  }
}

// ---- SPK11: Wii Remote speaker channel ----------------------------------------

void set_speaker_pump(SpeakerPump pump) { g_speaker_pump.store(pump, std::memory_order_relaxed); }

void push_speaker_samples(int channel, const int16_t *mono, size_t n, uint32_t rate, float gain) {
  if (channel < 0 || channel > 3 || !n || !rate) return;
  Out &o = out();
  std::call_once(o.once, init_once);
  std::lock_guard<std::mutex> lock(o.spk_mu);
  if (channel == 0 && !o.spk_wav_tried) {
    o.spk_wav_tried = true;
    if (const char *d = env("NWII_SPK_DUMP")) {
      o.spk_wav.f = std::fopen(d, "wb");
      o.spk_wav.rate = rate;
      o.spk_wav.ch = 1;
      if (o.spk_wav.f) {
        o.spk_wav.header();
        std::fprintf(stderr, "[SPK] dumping remote 0 speaker samples (%u Hz mono) to %s\n", rate, d);
      } else {
        std::fprintf(stderr, "[SPK] cannot open dump %s\n", d);
      }
    }
  }
  if (channel == 0 && o.spk_wav.f) {
    std::vector<uint8_t> le(n * 2);
    for (size_t i = 0; i < n; ++i) { le[2 * i] = (uint8_t)mono[i]; le[2 * i + 1] = (uint8_t)((uint16_t)mono[i] >> 8); }
    std::fwrite(le.data(), 1, le.size(), o.spk_wav.f);
    o.spk_wav.frames += n;
    if (o.spk_wav.frames - o.spk_wav.last_patch >= o.spk_wav.rate) {
      o.spk_wav.last_patch = o.spk_wav.frames;
      o.spk_wav.header();
    }
  }
  if (gain > 0.0f) o.spk[channel].push(mono, n, rate, gain);
}

SpeakerStats speaker_stats() {
  Out &o = out();
  std::lock_guard<std::mutex> lock(o.spk_mu);
  SpeakerStats s;
  for (const SpeakerMixer &m : o.spk) {
    const SpeakerStats &t = m.stats();
    s.pushed += t.pushed; s.mixed += t.mixed; s.gaps += t.gaps; s.dropped += t.dropped;
    s.frames_touched += t.frames_touched;
    s.fill += (uint32_t)m.avail();
  }
  return s;
}

float SpeakerMixer::at(size_t k) const {
  const size_t n = avail();
  if (!n) return last_;
  return fifo_[head_ + std::min(k, n - 1)];
}

void SpeakerMixer::push(const int16_t *mono, size_t n, uint32_t rate, float gain) {
  if (!n || !rate) return;
  rate_ = rate;
  if (head_ > 4096 && head_ * 2 > fifo_.size()) {  // compact the consumed prefix
    fifo_.erase(fifo_.begin(), fifo_.begin() + (std::ptrdiff_t)head_);
    head_ = 0;
  }
  for (size_t i = 0; i < n; ++i) fifo_.push_back(mono[i] * gain);
  st_.pushed += n;
  // A backlog beyond 250 ms (AI buffers skipped at a clock jump, or no AI DMA): keep the newest 100 ms.
  const size_t limit = rate / 4, keep = rate / 10;
  if (avail() > limit) {
    const size_t drop = avail() - keep;
    head_ += drop;
    st_.dropped += drop;
  }
}

size_t SpeakerMixer::mix_into(int16_t *lr, size_t frames, uint32_t out_rate) {
  if (idle() || !out_rate) return 0;
  const double step = (double)rate_ / out_rate;
  // Start a sound once two 20-byte reports (2 x 6.67 ms at 6 kHz) are queued, or after 10 ms of waiting
  // (a sound shorter than that); this absorbs the report/AI-buffer interleave without gaps.
  const size_t prebuffer = std::max<size_t>(3, (size_t)(rate_ * 0.0134));
  const uint32_t wait_limit = out_rate / 100;
  const float attack = 1000.0f / (float)out_rate;  // 1 ms ramp at the start of a sound
  fade_len_ = std::max<uint32_t>(1, out_rate / 200);  // 5 ms fade when the channel runs dry
  size_t touched = 0;
  for (size_t i = 0; i < frames; ++i) {
    if (!active_) {
      const size_t n = avail();
      if (n >= prebuffer || (n > 0 && waited_ >= wait_limit)) {
        active_ = true;
        frac_ = 0.0;
        xm1_ = at(0);
        waited_ = 0;
        fade_left_ = 0;
      } else if (n > 0) {
        ++waited_;
      }
    }
    float y;
    if (active_) {
      if (!avail()) {  // ran dry: fade out from the last output value
        active_ = false;
        ++st_.gaps;
        fade_from_ = last_;
        fade_left_ = fade_len_;
        env_ = 0.0f;
      }
    }
    if (active_) {
      const float x0 = at(0), x1 = at(1), x2 = at(2);
      const float f = (float)frac_;
      y = x0 + 0.5f * f * (x1 - xm1_ + f * (2.0f * xm1_ - 5.0f * x0 + 4.0f * x1 - x2 +
                                           f * (3.0f * (x0 - x1) + x2 - xm1_)));
      env_ = std::min(1.0f, env_ + attack);
      y *= env_;
      last_ = y;
      frac_ += step;
      while (frac_ >= 1.0 && avail()) {
        frac_ -= 1.0;
        xm1_ = fifo_[head_];
        ++head_;
        ++st_.mixed;
      }
    } else if (fade_left_) {
      y = fade_from_ * (float)--fade_left_ / (float)fade_len_;
      last_ = y;
    } else {
      continue;
    }
    for (int c = 0; c < 2; ++c) {
      const long v = (long)lr[2 * i + c] + std::lround(y);
      lr[2 * i + c] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    ++touched;
  }
  st_.frames_touched += touched;
  return touched;
}

}  // namespace nwii::runtime::audio
