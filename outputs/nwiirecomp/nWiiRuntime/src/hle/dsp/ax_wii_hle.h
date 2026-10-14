// AXWii DSP ucode HLE (AUDIO track, roadmap M5.1/M5.2).
//
// Re-implements the semantics of Dolphin's DSPHLE ROM ucode + AXWiiUCode
// (Source/Core/Core/HW/DSPHLE/UCodes/{ROM,AX,AXWii}.cpp, AXVoice.h,
// Core/DSP/DSPAccelerator.cpp) for the NEW AXWii command set that Skyward
// Sword's __AXNextFrame (0x8039CDA0) emits (0 SETUP .. 0xE END).
// PB layout: decomp include/rvl/AX/AXPB.h (0x140 bytes).
//
// The class is pure: it touches guest RAM only through DspMemory and returns
// DSP->CPU mails to the caller (hw_dsp.cpp owns mailbox, interrupts, timing),
// so it can be unit-tested (outputs/pc-tests/audio).
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace nwii::runtime::dsp {

// Physical-address view of MEM1/MEM2 (big-endian guest bytes).
struct DspMemory {
  uint8_t *mem1 = nullptr;
  uint32_t mem1_size = 0;
  uint8_t *mem2 = nullptr;
  uint32_t mem2_size = 0;

  // Host pointer for [phys, phys+len) or nullptr when unmapped.
  uint8_t *ptr(uint32_t phys, uint32_t len) const {
    phys &= 0x3FFFFFFFu;
    if (phys < mem1_size && len <= mem1_size - phys) return mem1 + phys;
    if (phys >= 0x10000000u) {
      const uint32_t o = phys - 0x10000000u;
      if (o < mem2_size && len <= mem2_size - o) return mem2 + o;
    }
    return nullptr;
  }
  uint8_t rd8(uint32_t a) const { const uint8_t *p = ptr(a, 1); return p ? p[0] : 0; }
  uint16_t rd16(uint32_t a) const {
    const uint8_t *p = ptr(a, 2);
    return p ? (uint16_t)((p[0] << 8) | p[1]) : 0;
  }
  uint32_t rd32(uint32_t a) const {
    const uint8_t *p = ptr(a, 4);
    return p ? ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3] : 0;
  }
  void wr16(uint32_t a, uint16_t v) const {
    if (uint8_t *p = ptr(a, 2)) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
  }
  void wr32(uint32_t a, uint32_t v) const {
    if (uint8_t *p = ptr(a, 4)) {
      p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
    }
  }
};

// DSP <-> CPU mail constants (Dolphin UCodes.h / AX.h).
enum : uint32_t {
  DSP_INIT = 0xDCD10000u,
  DSP_RESUME = 0xDCD10001u,
  DSP_YIELD = 0xDCD10002u,
  DSP_DONE = 0xDCD10003u,
  DSP_SYNC = 0xDCD10004u,
  MAIL_RESUME = 0xCDD10000u,
  MAIL_NEW_UCODE = 0xCDD10001u,
  MAIL_RESET = 0xCDD10002u,
  MAIL_CONTINUE = 0xCDD10003u,
  MAIL_CMDLIST = 0xBABE0000u,
};

// PB word indices (u16 units) — AXPB.h byte offsets / 2.
namespace pbw {
enum : int {
  NEXT_HI = 0, NEXT_LO = 1, SRC_SELECT = 4, COEF_SELECT = 5, MIXER_HI = 6, MIXER_LO = 7,
  STATE = 8, TYPE = 9,
  MIX = 10,          // 24 words: vL dL vR dR vAL dAL vAR dAR vBL dBL vBR dBR vCL dCL vCR dCR vS dS vAS dAS vBS dBS vCS dCS
  ITD = 34,          // 7 words (not emulated, as in Dolphin)
  DPOP = 41,         // 12 words: L AL BL CL R AR BR CR S AS BS CS
  VE_VOL = 53, VE_DELTA = 54,
  LOOP_FLAG = 55, FORMAT = 56, LOOP_HI = 57, LOOP_LO = 58, END_HI = 59, END_LO = 60, CUR_HI = 61, CUR_LO = 62,
  ADPCM_COEFS = 63,  // 16 words
  ADPCM_GAIN = 79, ADPCM_PS = 80, ADPCM_YN1 = 81, ADPCM_YN2 = 82,
  SRC_RATIO_HI = 83, SRC_RATIO_LO = 84, SRC_FRAC = 85, SRC_LAST = 86,  // 4 words
  LOOP_PS = 90, LOOP_YN1 = 91, LOOP_YN2 = 92,
  LPF_ON = 93, LPF_YN1 = 94, LPF_A0 = 95, LPF_B0 = 96,
  BQ_ON = 97, BQ_XN1 = 98, BQ_XN2 = 99, BQ_YN1 = 100, BQ_YN2 = 101, BQ_B0 = 102, BQ_B1 = 103, BQ_B2 = 104,
  BQ_A1 = 105, BQ_A2 = 106,
  REMOTE = 107, RMT_MIXER = 108,
  RMT_MIX = 109,     // 16 words: vM0 dM0 vA0 dA0 vM1 dM1 ...
  RMT_DPOP = 125,    // 8 words: M0 M1 M2 M3 A0 A1 A2 A3
  RMT_SRC_FRAC = 133, RMT_SRC_LAST = 134,  // 4 words
  RMT_IIR = 138,     // 10 words (lpf: on yn1 a0 b0 | biquad: on xn1 xn2 yn1 yn2 b0 b1 b2 a1 a2)
  COUNT = 160,       // 0x140 bytes
};
}

struct PB {
  uint16_t w[pbw::COUNT];
};
void read_pb(const DspMemory &m, uint32_t addr, PB &pb);
void write_pb(const DspMemory &m, uint32_t addr, const PB &pb);

// Accelerator (Dolphin DSPAccelerator.cpp ReadSample + AXVoice.h HLEAccelerator).
struct Accelerator {
  const DspMemory *mem = nullptr;
  PB *pb = nullptr;  // OnEndException target (loop info / running flag)
  uint32_t start = 0, end = 0, cur = 0;
  uint16_t format = 0, gain = 0, pred_scale = 0;
  int16_t yn1 = 0, yn2 = 0;
  bool reads_stopped = false;

  void setup(const DspMemory &m, PB &p);
  void store(PB &p) const;  // cur addr, yn1/yn2, pred_scale back to the PB
  int16_t read_sample();
};

enum SrcType : uint16_t { SRC_POLYPHASE = 0, SRC_LINEAR = 1, SRC_NEAREST = 2 };

// Dolphin ResampleAudio semantics. `coeffs` = 4-tap polyphase table for the
// selected coef set (128 phases x 4), or nullptr for the linear fallback.
// Returns the new 16.16 position (fraction only remains).
template <typename In>
uint32_t resample(In &&input, int16_t *out, uint32_t count, uint16_t *last, uint32_t pos,
                  uint32_t ratio, int type, const int16_t *coeffs);

// 4 coef sets x 128 phases x 4 taps (Q15). Built-in default is a 4-tap
// Catmull-Rom table; NWII_DSP_COEF=<dsp_coef.bin> loads the DSP DROM table
// (0x1000 bytes BE) exactly as Dolphin does.
const int16_t *polyphase_table();
bool load_polyphase_table(const char *path);

struct Mail {
  uint32_t value;
  bool irq;
};

struct AxStats {
  uint64_t frames = 0, voices = 0, cl_bytes = 0, outputs = 0;
  uint32_t last_voices = 0, max_voices = 0;
  uint32_t ucode_hash = 0;
  uint64_t unknown_cmds = 0;
  // Last 4 OUTPUT (L/R) destinations, physical. An AI DMA buffer elsewhere was
  // produced by the CPU (e.g. THPPlayer's AI callback mixing movie audio).
  uint32_t out_addr[4] = {};
  uint32_t out_addr_n = 0;
  bool is_output_addr(uint32_t phys) const {
    for (uint32_t a : out_addr) if (a && a == (phys & 0x3FFFFFE0u)) return true;
    return false;
  }
};

class AxWiiHle {
 public:
  explicit AxWiiHle(const DspMemory &mem) : m_mem(mem) {}
  void set_memory(const DspMemory &mem) { m_mem = mem; }

  // CSR reset / init: back to the ROM bootstrap state (no mail queued here).
  void reset_to_rom();
  bool in_ax() const { return m_state == State::Ax; }

  // One CPU->DSP mail. Replies are appended to `out` in order.
  void handle_mail(uint32_t mail, std::vector<Mail> &out);

  // Exposed for unit tests.
  void process_command_list(uint32_t addr, uint32_t words, std::vector<Mail> &out);
  void process_voice(PB &pb);  // 96 samples into the mixing buffers
  void clear_buffers();
  std::array<int32_t, 96> &main_left() { return m_main[0]; }
  std::array<int32_t, 96> &main_right() { return m_main[1]; }

  const AxStats &stats() const { return m_stats; }

  static uint32_t hash_ector(const uint8_t *p, uint32_t len);

 private:
  enum class State { Rom, Ax };
  enum Buf { MAIN_L, MAIN_R, MAIN_S, A_L, A_R, A_S, B_L, B_R, B_S, C_L, C_R, C_S, NBUF };

  void boot_ucode(std::vector<Mail> &out);
  void setup(uint32_t init_addr);
  void add_to_lr(uint32_t addr, bool neg);
  void add_sub_to_lr(uint32_t addr);
  void process_pb_list(uint32_t addr);
  void mix_aux(int aux, uint32_t write_addr, uint32_t read_addr, uint16_t volume);
  void upload_aux_mix_lrsc(int aux, const uint32_t *addrs, uint16_t volume);
  void compressor(uint16_t threshold, uint16_t release_frames, uint32_t table_addr);
  void output(uint32_t lr_addr, uint32_t surround_addr, uint16_t volume, bool dpl2, std::vector<Mail> &out);
  void wm_output(const uint32_t *addrs);

  DspMemory m_mem;
  State m_state = State::Rom;
  // ROM bootstrap parameters (Dolphin ROMUCode).
  uint32_t m_rom_next = 0, m_rom_ram_addr = 0, m_rom_len = 0;
  // AX mail state.
  bool m_next_is_cmdlist = false;
  uint16_t m_cmdlist_size = 0;
  int m_upload_steps = -1;  // >=0 while consuming NEW_UCODE parameters
  uint32_t m_upload_iram = 0, m_upload_iram_len = 0;

  std::array<std::array<int32_t, 96>, NBUF> m_main{};
  std::array<std::array<int32_t, 18>, 8> m_wm{};  // main0 aux0 main1 aux1 ...
  uint16_t m_last_aux_vol[3] = {0x8000, 0x8000, 0x8000};
  uint16_t m_last_main_vol = 0x8000;
  uint16_t m_compressor_pos = 0;
  AxStats m_stats;
};

// ---- template implementation ------------------------------------------------
template <typename In>
uint32_t resample(In &&input, int16_t *out, uint32_t count, uint16_t *last, uint32_t pos,
                  uint32_t ratio, int type, const int16_t *coeffs) {
  if (type == SRC_POLYPHASE || type == SRC_LINEAR) {
    int16_t t[4];
    uint32_t idx = 0;
    for (int i = 0; i < 4; ++i) t[idx++ & 3] = (int16_t)last[i];
    for (uint32_t i = 0; i < count; ++i) {
      pos += ratio;
      while (pos >= 0x10000) {
        t[idx++ & 3] = input();
        pos -= 0x10000;
      }
      if (coeffs && type == SRC_POLYPHASE) {
        const int16_t *c = &coeffs[((pos & 0xFFFF) >> 9) << 2];
        const int64_t s = ((int64_t)t[idx & 3] * c[0] + (int64_t)t[(idx + 1) & 3] * c[1] +
                           (int64_t)t[(idx + 2) & 3] * c[2] + (int64_t)t[(idx + 3) & 3] * c[3]) >> 15;
        out[i] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
      } else {
        const uint32_t frac = pos & 0xFFFF;
        if (frac) {
          const int32_t s0 = t[idx & 3], s1 = t[(idx + 1) & 3];
          out[i] = (int16_t)((s0 * (int32_t)(uint16_t)(0x10000 - frac) + s1 * (int32_t)frac) >> 16);
        } else {
          out[i] = t[idx & 3];
        }
      }
    }
    // last[] = the 4 most recent inputs, oldest first.
    for (int i = 0; i < 4; ++i) last[i] = (uint16_t)t[(idx + i) & 3];
    return pos;
  }
  for (uint32_t i = 0; i < count; ++i) out[i] = input();
  if (count >= 4)
    for (int i = 0; i < 4; ++i) last[i] = (uint16_t)out[count - 4 + i];
  return pos;
}

}  // namespace nwii::runtime::dsp
