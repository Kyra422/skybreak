// Host audio output for the AI DMA stream (AUDIO track, roadmap M5.3).
//
// The guest pushes what the AI DMA engine plays, in VIRTUAL time (hw_dsp.cpp,
// once per DMA buffer reload). An SDL callback device (default 48 kHz) pulls
// from a ring buffer through a 4-point cubic resampler whose ratio is nudged by
// at most +-0.5 % to hold a latency target (drift correction). If the game
// runs slower than real time the ring drains: we output silence (short fade)
// and re-buffer to the target instead of pitch-shifting/stretching, so audio
// never desyncs from the game; if it runs faster the oldest excess is dropped.
//
// Env (read once): NWII_AUDIO=0 disables the host device (emulation unchanged;
// default on, except under NWII_HEADLESS=1 where NWII_AUDIO=1 opts in),
// NWII_AUDIO_DUMP=<file.wav> writes the pushed stream (source rate, s16 LE
// stereo L/R), NWII_AUDIO_LATENCY_MS (default 80), NWII_AUDIO_STATS=1 prints a
// status line every 10 s of pushed audio.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace nwii::runtime::audio {

// `frames` frames of interleaved host-order L,R int16 at `rate` Hz.
void push_samples(const int16_t *lr, size_t frames, uint32_t rate);

struct OutStats {
  uint64_t pushed = 0, played = 0, underruns = 0, dropped = 0;
  uint32_t fill = 0, device_rate = 0;
  bool device_open = false;
  uint32_t latency_ms = 0;  // LAUNCH19: the current latency target (apply_host_settings)
};
OutStats stats();

// Finalises the WAV dump (also done automatically at exit).
void shutdown();

// LAUNCH19: output gain 0..1 (master volume x "mute when SkyBreak is not active"), applied to the device output with
// a one-callback ramp; the WAV dump keeps the pushed stream. NWII_AUDIO_DEVICE=<name> picks the output device
// ([audio] device; unknown = Windows default).
void set_output_gain(float gain);
float output_gain();
// LAUNCH19: re-reads NWII_AUDIO_LATENCY_MS (new latency target) and NWII_AUDIO_DEVICE (reopens an open device when
// the name changed). Called when the game starts: the launcher music may have opened the device with older values.
void apply_host_settings();

// ---- resampler core, exposed for unit tests --------------------------------
// `in` holds `in_frames` interleaved stereo frames; in[0] is the history frame
// x[-1] and the output position is x[0] + *frac. Produces up to `out_frames`
// frames, advancing *frac by `step` per output frame, and stops when fewer than
// 4 frames (x[-1..2]) remain. Returns frames produced; *consumed = whole input
// frames that may be dropped from the front.
size_t resample_cubic(const int16_t *in, size_t in_frames, double *frac, double step, int16_t *out,
                      size_t out_frames, size_t *consumed);

// ---- Wii Remote speaker channel (SPK11, roadmap M5.4) ------------------------
// The WPAD speaker HLE (src/hle/pad.cpp) decodes the stream the game sends to a
// remote (Dolphin SpeakerLogic, src/input/wiimote_speaker.h) and pushes the mono
// samples here in VIRTUAL time. push_samples() mixes every remote's channel into
// the AI buffer before the dump and the host ring, so the speaker stays in sync
// with the game audio and shares the device, drift correction and underrun
// handling. `gain` = Dolphin's register volume (0..255)/256 times [audio]
// remote_speaker_volume. Env (read once): NWII_SPK_DUMP=<file.wav> writes remote
// 0's decoded samples (mono s16 at the speaker rate, before gain).
void push_speaker_samples(int channel, const int16_t *mono, size_t n, uint32_t rate, float gain);

// Diagnostic pump (NWII_SPK_SELFTEST, pad.cpp): called at the start of every push_samples() with the AI
// buffer's frame count and rate, i.e. paced in virtual time. nullptr (default) = off.
using SpeakerPump = void (*)(size_t frames, uint32_t rate);
void set_speaker_pump(SpeakerPump pump);

struct SpeakerStats {
  uint64_t pushed = 0;   // speaker samples received
  uint64_t mixed = 0;    // speaker samples consumed into the output
  uint64_t gaps = 0;     // times the channel ran dry while playing (end of a sound or a late packet)
  uint64_t dropped = 0;  // samples dropped because the backlog exceeded 100 ms
  uint64_t frames_touched = 0;  // output frames that received speaker signal
  uint32_t fill = 0;     // samples queued now
};
SpeakerStats speaker_stats();  // all remotes summed

// One remote's channel; exposed for unit tests. Not thread-safe (audio_out locks around it).
class SpeakerMixer {
public:
  void push(const int16_t *mono, size_t n, uint32_t rate, float gain);
  // Adds this channel into `frames` interleaved stereo frames at `out_rate` (saturating); returns frames touched.
  size_t mix_into(int16_t *lr, size_t frames, uint32_t out_rate);
  bool idle() const { return !active_ && fade_left_ == 0 && avail() == 0; }
  size_t avail() const { return fifo_.size() - head_; }
  uint32_t rate() const { return rate_; }
  const SpeakerStats &stats() const { return st_; }

private:
  float at(size_t k) const;  // fifo_[head_ + k], holding the newest sample past the end
  std::vector<float> fifo_;
  size_t head_ = 0;
  uint32_t rate_ = 6000;
  bool active_ = false;
  double frac_ = 0.0;
  float xm1_ = 0.0f, env_ = 0.0f, last_ = 0.0f, fade_from_ = 0.0f;
  uint32_t fade_left_ = 0, fade_len_ = 1, waited_ = 0;
  SpeakerStats st_;
};

}  // namespace nwii::runtime::audio
