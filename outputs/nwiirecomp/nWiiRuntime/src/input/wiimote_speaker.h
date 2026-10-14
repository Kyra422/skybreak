// Wii Remote speaker emulation (SPK11, roadmap M5.4). Header-only so it is unit-testable without the
// runtime (outputs/pc-tests/spk11). The WPAD hooks that drive it live in src/hle/pad.cpp; the decoded
// samples are mixed into the host output by src/platform/audio_out.cpp (push_speaker_samples).
//
// Dolphin reference (Source/Core/Core/HW/WiimoteEmu/Speaker.{h,cpp}, Wiimote.cpp):
//   * the speaker is an I2C slave (0x51) with a 0x100-byte register file (packed Register struct:
//     0 speaker_data, 1 unk_1, 2 format, 3..4 sample_rate little-endian, 5 volume, 6.. unknown);
//   * output report 0x14 sets "speaker enabled", report 0x19 sets "speaker muted";
//   * output report 0x18 (speaker data, length in the top 5 bits of the first byte, <= 20 bytes) is dropped
//     while muted, else written to register 0x00, which decodes it (SpeakerData): nothing while disabled or
//     with sample_rate 0; format 0x00 = 4-bit Yamaha ADPCM (high nibble first, 6 MHz / rate, x2), format
//     0x40 = signed 8-bit PCM (12 MHz / rate, x2); volume = reg.volume * 256 / divisor (0x7F ADPCM, 0xFF
//     PCM; a larger register value becomes the divisor), clamped to 255, applied as /256;
//   * the ADPCM decoder state is only reset by SpeakerLogic::Reset (controller reset), not by register writes.
//
// What the SDK sends (SOUE01 WPAD.o, decoded from WPADControlSpeaker 803DBA40 and WPADSendStreamData
// 803DD2A0; the HLE applies the same reports to this model instead of queueing them on Bluetooth):
//   OFF(0):      [only if enabled] 0x19<-04 (mute), write A20001<-01, write A20009<-00, 0x14<-00 (disable), status
//   ON(1), (5):  0x14<-04 (enable), 0x19<-04, write A20009<-01, write A20001<-80,
//                write A20001<-{conf[7] = 00 00 D0 07 <WPAD speaker volume> 0C 0E}, 0x19<-00 (unmute), status
//   MUTE(2):     0x19<-04, status          UNMUTE(3): 0x19<-00, status          PLAY(4): write A20008<-01, status
//   stream:      report 0x18 = {(len & 0x1F) << 3, len bytes}
// The conf template is the game's own .sdata2 bytes (8057E930..36); byte 4 is the SDK speaker volume byte.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace nwii::runtime::speaker {

constexpr uint8_t kFormatAdpcm = 0x00;
constexpr uint8_t kFormatPcm8 = 0x40;
constexpr int kMaxReportData = 20;                 // OutputReportSpeakerData::DATA_SIZE
constexpr int kMaxSamplesPerReport = 2 * kMaxReportData;

// Register offsets (Dolphin SpeakerLogic::Register, packed).
constexpr uint8_t kRegData = 0x00, kRegFormat = 0x02, kRegRateLo = 0x03, kRegRateHi = 0x04,
                  kRegVolume = 0x05;

// WPADControlSpeaker commands (RVL SDK).
constexpr uint32_t kSpeakerOff = 0, kSpeakerOn = 1, kSpeakerMute = 2, kSpeakerUnmute = 3, kSpeakerPlay = 4,
                   kSpeakerOn5 = 5;

// ---- Yamaha ADPCM decoder (Dolphin Speaker.cpp, from ffmpeg) -------------------------------------------
struct AdpcmState {
  int32_t predictor = 0;
  int32_t step = 127;
};

inline int16_t av_clip16(int32_t a) {
  if ((a + 32768) & ~65535) return static_cast<int16_t>((a >> 31) ^ 32767);
  return static_cast<int16_t>(a);
}

inline int32_t av_clip(int32_t a, int32_t amin, int32_t amax) { return a < amin ? amin : a > amax ? amax : a; }

inline int16_t adpcm_yamaha_expand_nibble(AdpcmState& s, uint8_t nibble) {
  static constexpr int32_t kDiffLookup[16] = {1, 3, 5, 7, 9, 11, 13, 15, -1, -3, -5, -7, -9, -11, -13, -15};
  static constexpr int32_t kIndexScale[16] = {230, 230, 230, 230, 307, 409, 512, 614,
                                              230, 230, 230, 230, 307, 409, 512, 614};
  nibble &= 0xF;
  s.predictor += (s.step * kDiffLookup[nibble]) / 8;  // C division: truncates toward zero
  s.predictor = av_clip16(s.predictor);
  s.step = (s.step * kIndexScale[nibble]) >> 8;
  s.step = av_clip(s.step, 127, 24576);
  return static_cast<int16_t>(s.predictor);
}

// ---- speaker model (Dolphin SpeakerLogic + the Wiimote's speaker enable/mute flags) ----------------------
struct SpeakerLogic {
  std::array<uint8_t, 0x100> reg{};
  AdpcmState adpcm{};
  bool enabled = false;  // output report 0x14
  bool muted = false;    // output report 0x19

  // Dolphin SpeakerLogic::Reset (+ the Wiimote flags of a freshly connected remote).
  void reset() {
    reg.fill(0);
    adpcm = AdpcmState{};
    enabled = false;
    muted = false;
  }
  uint8_t format() const { return reg[kRegFormat]; }
  uint16_t sample_rate_reg() const { return static_cast<uint16_t>(reg[kRegRateLo] | (reg[kRegRateHi] << 8)); }
  uint8_t volume() const { return reg[kRegVolume]; }

  // I2CSlave::RawWrite: copies up to the end of the register file; returns bytes written.
  int raw_write(uint8_t addr, const uint8_t* data, int count) {
    count = std::min(count, 0x100 - int(addr));
    for (int i = 0; i < count; ++i) reg[size_t(addr) + size_t(i)] = data[i];
    return count < 0 ? 0 : count;
  }
};

// One SpeakerData call's output (what Dolphin hands to Mixer::PushWiimoteSpeakerSamples).
struct SpeakerBlock {
  std::array<int16_t, kMaxSamplesPerReport> samples{};
  size_t count = 0;
  uint32_t rate = 0;     // output sample rate in Hz (sample_rate * 2, as Dolphin pushes it)
  uint32_t volume = 0;   // 0..255 (Dolphin l_volume == r_volume at pan 0), applied as volume / 256
  float gain() const { return float(volume) / 256.0f; }
};

// Dolphin SpeakerLogic::SpeakerData with speaker_pan = 0. Returns false when nothing is produced.
inline bool speaker_data(SpeakerLogic& s, const uint8_t* data, int length, SpeakerBlock* out) {
  if (!s.enabled) return false;
  if (s.sample_rate_reg() == 0 || length <= 0) return false;
  length = std::min(length, kMaxReportData);
  uint32_t dividend;
  uint32_t divisor;
  size_t count;
  if (s.format() == kFormatPcm8) {
    for (int i = 0; i < length; ++i) out->samples[size_t(i)] = int16_t(int16_t(int8_t(data[i])) * 0x100);
    dividend = 12000000;
    divisor = 0xFF;
    count = size_t(length);
  } else if (s.format() == kFormatAdpcm) {
    for (int i = 0; i < length; ++i) {
      out->samples[size_t(i) * 2] = adpcm_yamaha_expand_nibble(s.adpcm, uint8_t((data[i] >> 4) & 0xF));
      out->samples[size_t(i) * 2 + 1] = adpcm_yamaha_expand_nibble(s.adpcm, uint8_t(data[i] & 0xF));
    }
    dividend = 6000000;
    divisor = 0x7F;
    count = size_t(length) * 2;
  } else {
    return false;  // Dolphin: "Unknown speaker format"
  }
  if (s.volume() > divisor) divisor = s.volume();
  const double volume = float(s.volume()) * 256.f / float(divisor);
  out->volume = std::min(uint32_t(volume), 255u);
  out->rate = (dividend / s.sample_rate_reg()) * 2;
  out->count = count;
  return true;
}

enum class ReportResult { Decoded, Muted, BadLength, Silent };

// Output report 0x18 (Wiimote::HandleSpeakerData): `length` = first payload byte >> 3.
inline ReportResult speaker_report(SpeakerLogic& s, uint8_t length, const uint8_t* data, SpeakerBlock* out) {
  if (s.muted) return ReportResult::Muted;
  if (length > kMaxReportData) return ReportResult::BadLength;
  return speaker_data(s, data, length, out) ? ReportResult::Decoded : ReportResult::Silent;
}

// Applies the reports the SDK's WPADControlSpeaker queues for `command` (see the table at the top).
// `conf` = the 7-byte configuration with the SDK speaker volume already in conf[4].
// Returns false for an unknown command (the SDK then queues nothing and reports its status).
inline bool wpad_control_speaker(SpeakerLogic& s, uint32_t command, const uint8_t conf[7]) {
  const uint8_t one = 0x01, zero = 0x00, x80 = 0x80;
  switch (command) {
  case kSpeakerOff:
    if (!s.enabled) return true;  // SDK: already off -> WPAD_ERR_OK, no reports
    s.muted = true;
    s.raw_write(0x01, &one, 1);
    s.raw_write(0x09, &zero, 1);
    s.enabled = false;
    return true;
  case kSpeakerOn:
  case kSpeakerOn5:
    s.enabled = true;
    s.muted = true;
    s.raw_write(0x09, &one, 1);
    s.raw_write(0x01, &x80, 1);
    s.raw_write(0x01, conf, 7);
    s.muted = false;
    return true;
  case kSpeakerMute:
    s.muted = true;
    return true;
  case kSpeakerUnmute:
    s.muted = false;
    return true;
  case kSpeakerPlay:
    s.raw_write(0x08, &one, 1);
    return true;
  default:
    return false;
  }
}

// Yamaha ADPCM encoder (ffmpeg adpcm_yamaha_compress_sample), the inverse of adpcm_yamaha_expand_nibble.
// Only used by the NWII_SPK_SELFTEST diagnostic (pad.cpp); the game encodes with its own WENC library.
inline uint8_t adpcm_yamaha_compress_sample(AdpcmState& c, int16_t sample) {
  static constexpr int32_t kDiffLookup[16] = {1, 3, 5, 7, 9, 11, 13, 15, -1, -3, -5, -7, -9, -11, -13, -15};
  static constexpr int32_t kIndexScale[16] = {230, 230, 230, 230, 307, 409, 512, 614,
                                              230, 230, 230, 230, 307, 409, 512, 614};
  const int32_t delta = int32_t(sample) - c.predictor;
  const int32_t mag = (delta < 0 ? -delta : delta) * 4 / c.step;
  const uint8_t nibble = uint8_t((mag < 7 ? mag : 7) + (delta < 0 ? 8 : 0));
  c.predictor = av_clip16(c.predictor + (c.step * kDiffLookup[nibble]) / 8);
  c.step = av_clip((c.step * kIndexScale[nibble]) >> 8, 127, 24576);
  return nibble;
}

inline const char* command_name(uint32_t command) {
  switch (command) {
  case kSpeakerOff: return "OFF";
  case kSpeakerOn: return "ON";
  case kSpeakerMute: return "MUTE";
  case kSpeakerUnmute: return "UNMUTE";
  case kSpeakerPlay: return "PLAY";
  case kSpeakerOn5: return "ON(5)";
  default: return "?";
  }
}

}  // namespace nwii::runtime::speaker
