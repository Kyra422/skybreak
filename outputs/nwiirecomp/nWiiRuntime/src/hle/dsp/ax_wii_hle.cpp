// AXWii DSP ucode HLE — see ax_wii_hle.h for scope and references.
#include "ax_wii_hle.h"

#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>

namespace nwii::runtime::dsp {

namespace {

inline int16_t clamp16(int64_t v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

// SPK11 diagnostics (NWII_SPK_STATS=1): how much the game routes to the Wii Remote speaker buses. Printed
// every 3333 AX frames (~10 s of AX time): voice-frames with the REMOTE flag, and per remote the WM_OUTPUT
// frames carrying a non-zero sample and the peak. Read-only; no effect on the mix.
struct WmActivity {
  bool on = [] { const char *e = std::getenv("NWII_SPK_STATS"); return e && e[0] == '1'; }();
  uint64_t frames = 0, remote_voice_frames = 0, nonzero[4] = {}, last_remote_voice_frames = 0;
  int peak[4] = {};
};
WmActivity &wm_activity() {
  static WmActivity a;
  return a;
}
inline uint32_t hilo(uint16_t hi, uint16_t lo) { return ((uint32_t)hi << 16) | lo; }

// Dolphin AXWiiUCode::GenerateVolumeRamp.
void volume_ramp(uint16_t *out, uint16_t v1, uint16_t v2, size_t n) {
  float curr = v1;
  for (size_t i = 0; i < n; ++i) {
    curr += ((int)v2 - (int)v1) / (float)n;
    out[i] = (uint16_t)curr;
  }
}

// Dolphin AXVoice.h MixAdd: per-sample volume (u16, >>15), optional ramp,
// dpop = last mixed sample.
template <size_t N>
void mix_add(int32_t *out, const int16_t *in, uint16_t &vol, uint16_t delta, uint16_t &dpop, bool ramp) {
  if (!ramp) delta = 0;
  int16_t s16 = 0;
  for (size_t i = 0; i < N; ++i) {
    s16 = clamp16(((int64_t)in[i] * vol) >> 15);
    out[i] += s16;
    vol = (uint16_t)(vol + delta);
  }
  dpop = (uint16_t)s16;
}

void lowpass(int16_t *s, uint32_t n, uint16_t *w /* on yn1 a0 b0 */) {
  int16_t yn1 = (int16_t)w[1];
  const int32_t a0 = w[2], b0 = w[3];
  for (uint32_t i = 0; i < n; ++i) s[i] = yn1 = clamp16((a0 * (int32_t)s[i] + b0 * (int32_t)yn1) >> 15);
  w[1] = (uint16_t)yn1;
}

void biquad(int16_t *s, uint32_t n, uint16_t *w /* on xn1 xn2 yn1 yn2 b0 b1 b2 a1 a2 */) {
  int16_t xn1 = (int16_t)w[1], xn2 = (int16_t)w[2], yn1 = (int16_t)w[3], yn2 = (int16_t)w[4];
  const int32_t b0 = (int16_t)w[5], b1 = (int16_t)w[6], b2 = (int16_t)w[7], a1 = (int16_t)w[8], a2 = (int16_t)w[9];
  for (uint32_t i = 0; i < n; ++i) {
    const int16_t x0 = s[i];
    int64_t t = (int64_t)b0 * x0 + (int64_t)b1 * xn1 + (int64_t)b2 * xn2 + (int64_t)a1 * yn1 + (int64_t)a2 * yn2;
    t <<= 2;
    t += (t & 0x10000) ? 0x8000 : 0x7FFF;
    t >>= 16;
    const int16_t y0 = clamp16(t);
    xn2 = xn1; yn2 = yn1; xn1 = x0; yn1 = y0;
    s[i] = y0;
  }
  w[1] = (uint16_t)xn1; w[2] = (uint16_t)xn2; w[3] = (uint16_t)yn1; w[4] = (uint16_t)yn2;
}

struct CoefTable {
  int16_t t[4 * 128 * 4];
  CoefTable() {
    // 4-tap Catmull-Rom between t1 and t2, 128 phases, Q15 (sum forced to 0x8000).
    for (int p = 0; p < 128; ++p) {
      const double f = p / 128.0, f2 = f * f, f3 = f2 * f;
      const double w[4] = {(-f3 + 2 * f2 - f) / 2, (3 * f3 - 5 * f2 + 2) / 2, (-3 * f3 + 4 * f2 + f) / 2,
                           (f3 - f2) / 2};
      int16_t q[4];
      int sum = 0;
      for (int k = 0; k < 4; ++k) { q[k] = (int16_t)std::lround(w[k] * 32768.0 > 32767 ? 32767 : w[k] * 32768.0); sum += q[k]; }
      q[1] = (int16_t)(q[1] + (32768 - sum) > 32767 ? 32767 : q[1] + (32768 - sum));
      for (int set = 0; set < 4; ++set)
        for (int k = 0; k < 4; ++k) t[set * 512 + p * 4 + k] = q[k];
    }
  }
};
CoefTable g_coefs;

}  // namespace

const int16_t *polyphase_table() { return g_coefs.t; }

bool load_polyphase_table(const char *path) {
  if (!path || !*path) return false;
  FILE *f = std::fopen(path, "rb");
  if (!f) return false;
  uint8_t raw[0x1000];
  const size_t n = std::fread(raw, 1, sizeof(raw), f);
  std::fclose(f);
  if (n != sizeof(raw)) return false;
  for (int i = 0; i < 0x800; ++i) g_coefs.t[i] = (int16_t)((raw[2 * i] << 8) | raw[2 * i + 1]);
  return true;
}

void read_pb(const DspMemory &m, uint32_t addr, PB &pb) {
  const uint8_t *p = m.ptr(addr, pbw::COUNT * 2);
  if (!p) { std::memset(&pb, 0, sizeof(pb)); return; }
  for (int i = 0; i < pbw::COUNT; ++i) pb.w[i] = (uint16_t)((p[2 * i] << 8) | p[2 * i + 1]);
}

void write_pb(const DspMemory &m, uint32_t addr, const PB &pb) {
  uint8_t *p = m.ptr(addr, pbw::COUNT * 2);
  if (!p) return;
  for (int i = 0; i < pbw::COUNT; ++i) { p[2 * i] = (uint8_t)(pb.w[i] >> 8); p[2 * i + 1] = (uint8_t)pb.w[i]; }
}

// ---- accelerator ---------------------------------------------------------------
void Accelerator::setup(const DspMemory &m, PB &p) {
  mem = &m;
  pb = &p;
  start = hilo(p.w[pbw::LOOP_HI], p.w[pbw::LOOP_LO]);
  end = hilo(p.w[pbw::END_HI], p.w[pbw::END_LO]);
  cur = hilo(p.w[pbw::CUR_HI], p.w[pbw::CUR_LO]);
  format = p.w[pbw::FORMAT];
  yn1 = (int16_t)p.w[pbw::ADPCM_YN1];
  yn2 = (int16_t)p.w[pbw::ADPCM_YN2];
  gain = p.w[pbw::ADPCM_GAIN];
  pred_scale = p.w[pbw::ADPCM_PS] & 0x7F;
  reads_stopped = false;  // SetYn1/SetYn2 resume reads
}

void Accelerator::store(PB &p) const {
  p.w[pbw::CUR_HI] = (uint16_t)(cur >> 16);
  p.w[pbw::CUR_LO] = (uint16_t)cur;
  p.w[pbw::ADPCM_YN1] = (uint16_t)yn1;
  p.w[pbw::ADPCM_YN2] = (uint16_t)yn2;
  p.w[pbw::ADPCM_PS] = pred_scale;
}

int16_t Accelerator::read_sample() {
  if (reads_stopped) return 0;
  const int size = format & 3, decode = (format >> 2) & 3, gscale = (format >> 4) & 3;
  int16_t val;
  if (decode == 0) {  // ADPCM (4-bit)
    if ((cur & 15) == 0) {
      pred_scale = mem->rd8((cur & ~15u) >> 1) & 0x7F;
      cur += 2;
    }
    const uint8_t b = mem->rd8(cur >> 1);
    int nib = (cur & 1) ? (b & 0xF) : (b >> 4);
    if (nib >= 8) nib -= 16;
    const int ci = (pred_scale >> 4) & 7;
    const int32_t c1 = (int16_t)pb->w[pbw::ADPCM_COEFS + ci * 2], c2 = (int16_t)pb->w[pbw::ADPCM_COEFS + ci * 2 + 1];
    const int32_t v = (1 << (pred_scale & 0xF)) * nib + ((0x400 + c1 * yn1 + c2 * yn2) >> 11);
    val = clamp16(v);
    yn2 = yn1;
    yn1 = val;
    cur += 1;
  } else {  // PCM (gain applies only here)
    int32_t raw;
    if (size == 1) raw = (int8_t)mem->rd8(cur);
    else if (size == 2) raw = (int16_t)mem->rd16(cur * 2);
    else { const uint8_t b = mem->rd8(cur >> 1); raw = (cur & 1) ? (b & 0xF) : (b >> 4); }
    const int shift = gscale == 1 ? 0 : gscale == 2 ? 16 : 11;
    const int ci = (pred_scale >> 4) & 7;
    const int32_t c1 = (int16_t)pb->w[pbw::ADPCM_COEFS + ci * 2], c2 = (int16_t)pb->w[pbw::ADPCM_COEFS + ci * 2 + 1];
    const int32_t v = (((int32_t)gain * raw) >> shift) + ((c1 * yn1) >> shift) + ((c2 * yn2) >> shift);
    val = (int16_t)v;
    yn2 = yn1;
    yn1 = val;
    if (decode != 1) cur += 1;
  }
  if (cur == end + 1) {
    cur = start;
    reads_stopped = true;
    // HLEAccelerator::OnEndException
    if (pb->w[pbw::LOOP_FLAG]) {
      pred_scale = pb->w[pbw::LOOP_PS] & 0x7F;
      if (!pb->w[pbw::TYPE]) {  // not a stream: reload loop history
        yn1 = (int16_t)pb->w[pbw::LOOP_YN1];
        yn2 = (int16_t)pb->w[pbw::LOOP_YN2];
      }
      reads_stopped = false;
    } else {
      pb->w[pbw::STATE] = 0;  // one-shot reached the end -> stop
    }
  }
  return val;
}

// ---- ucode -------------------------------------------------------------------------
uint32_t AxWiiHle::hash_ector(const uint8_t *p, uint32_t len) {
  uint32_t crc = 0;
  for (uint32_t i = 0; i < len; ++i) {
    crc ^= p[i];
    crc = (crc << 3) | (crc >> 29);
  }
  return crc;
}

void AxWiiHle::reset_to_rom() {
  m_state = State::Rom;
  m_rom_next = 0;
  m_next_is_cmdlist = false;
  m_cmdlist_size = 0;
  m_upload_steps = -1;
}

void AxWiiHle::clear_buffers() {
  for (auto &b : m_main) b.fill(0);
  for (auto &b : m_wm) b.fill(0);
}

void AxWiiHle::boot_ucode(std::vector<Mail> &out) {
  const uint8_t *code = m_mem.ptr(m_rom_ram_addr, m_rom_len);
  m_stats.ucode_hash = code ? hash_ector(code, m_rom_len) : 0;
  std::fprintf(stderr, "[AXHLE] ucode boot: iram@%08x len=%u ector=%08x%s -> AXWii (new command set)\n",
               m_rom_ram_addr, m_rom_len, m_stats.ucode_hash,
               (m_stats.ucode_hash == 0xfa450138u || m_stats.ucode_hash == 0x7699af32u) ? " (OLD AXWii crc!)" : "");
  m_state = State::Ax;
  m_next_is_cmdlist = false;
  m_upload_steps = -1;
  m_last_main_vol = 0x8000;
  for (auto &v : m_last_aux_vol) v = 0x8000;
  m_compressor_pos = 0;
  clear_buffers();
  out.push_back({DSP_INIT, true});
}

void AxWiiHle::handle_mail(uint32_t mail, std::vector<Mail> &out) {
  if (m_state == State::Rom) {  // Dolphin ROMUCode::HandleMail
    if (m_rom_next == 0) {
      if ((mail & 0xFFFF0000u) != 0x80F30000u) out.push_back({0xFEEE0000u | (mail & 0xFFFF), false});
      else m_rom_next = mail;
      return;
    }
    switch (m_rom_next) {
    case 0x80F3A001u: m_rom_ram_addr = mail; break;
    case 0x80F3A002u: m_rom_len = mail & 0xFFFF; break;
    case 0x80F3D001u: m_rom_next = 0; boot_ucode(out); return;
    default: break;  // B002 dmem length, C002 imem address: not needed by HLE
    }
    m_rom_next = 0;
    return;
  }
  // Dolphin AXUCode::HandleMail
  if (m_next_is_cmdlist) {
    m_next_is_cmdlist = false;
    process_command_list(mail, m_cmdlist_size, out);
    out.push_back({DSP_YIELD, true});
    return;
  }
  if (m_upload_steps >= 0) {  // NEW_UCODE parameters (UCodeInterface::PrepareBootUCode)
    if (m_upload_steps == 3) m_upload_iram = mail;
    if (m_upload_steps == 4) m_upload_iram_len = mail & 0xFFFF;
    if (++m_upload_steps == 10) {
      m_upload_steps = -1;
      const uint8_t *code = m_mem.ptr(m_upload_iram, m_upload_iram_len);
      std::fprintf(stderr, "[AXHLE] NEW_UCODE iram@%08x len=%u ector=%08x: staying AXWii, sending DSP_RESUME\n",
                   m_upload_iram, m_upload_iram_len, code ? hash_ector(code, m_upload_iram_len) : 0);
      out.push_back({DSP_RESUME, true});
    }
    return;
  }
  if (mail == MAIL_RESUME) { out.push_back({DSP_RESUME, true}); return; }
  if (mail == MAIL_NEW_UCODE) { m_upload_steps = 0; return; }
  if (mail == MAIL_RESET) { reset_to_rom(); out.push_back({0x8071FEEDu, false}); return; }
  if (mail == MAIL_CONTINUE) return;  // no ACK: the CPU sends the next CL directly
  if ((mail & 0xFFFF0000u) == MAIL_CMDLIST) {
    m_next_is_cmdlist = true;
    m_cmdlist_size = (uint16_t)(mail & 0xFFFF);
    return;
  }
  std::fprintf(stderr, "[AXHLE] unknown mail %08x\n", mail);
}

void AxWiiHle::process_command_list(uint32_t addr, uint32_t words, std::vector<Mail> &out) {
  if (words == 0 || words > 0x1000) words = 0x1000;
  std::vector<uint16_t> cl(words);
  for (uint32_t i = 0; i < words; ++i) cl[i] = m_mem.rd16(addr + 2 * i);
  m_stats.frames++;
  m_stats.cl_bytes += words * 2;
  m_stats.last_voices = 0;
  uint32_t i = 0;
  auto arg = [&]() -> uint16_t { return i < words ? cl[i++] : 0; };
  auto arg32 = [&]() -> uint32_t { const uint16_t hi = arg(); return hilo(hi, arg()); };
  while (i < words) {
    const uint16_t cmd = cl[i++];
    switch (cmd) {
    case 0x00: setup(arg32()); break;
    case 0x01:
    case 0x02: add_to_lr(arg32(), cmd == 0x02); break;
    case 0x03: add_sub_to_lr(arg32()); break;
    case 0x04: process_pb_list(arg32()); break;
    case 0x05:
    case 0x06:
    case 0x07: {
      const uint16_t vol = arg();
      const uint32_t w = arg32();
      const uint32_t r = arg32();
      mix_aux(cmd - 0x05, w, r, vol);
      break;
    }
    case 0x08:
    case 0x09: {
      const uint16_t vol = arg();
      uint32_t a[6];
      for (auto &x : a) x = arg32();
      upload_aux_mix_lrsc(cmd == 0x09, a, vol);
      break;
    }
    case 0x0A: {
      const uint16_t threshold = arg(), frames = arg();
      compressor(threshold, frames, arg32());
      break;
    }
    case 0x0B:
    case 0x0C: {
      const uint16_t vol = arg();
      const uint32_t surround = arg32();
      const uint32_t lr = arg32();
      output(lr, surround, vol, cmd == 0x0C, out);
      break;
    }
    case 0x0D: {
      uint32_t a[4];
      for (auto &x : a) x = arg32();
      wm_output(a);
      break;
    }
    case 0x0E: return;
    default:
      m_stats.unknown_cmds++;
      return;  // cannot know the argument count: stop like a desynced ucode would
    }
  }
}

void AxWiiHle::setup(uint32_t init_addr) {
  // InitMixingBuffers<3ms>: per buffer {value hi, value lo, delta}; 12 x 96 + 8 x 18 samples.
  for (int b = 0; b < 20; ++b) {
    const uint32_t a = init_addr + b * 6;
    const int32_t value = (int32_t)hilo(m_mem.rd16(a), m_mem.rd16(a + 2));
    const int16_t delta = (int16_t)m_mem.rd16(a + 4);
    int32_t *buf;
    uint32_t n;
    if (b < 12) { buf = m_main[b].data(); n = 96; }
    else { buf = m_wm[b - 12].data(); n = 18; }
    for (uint32_t j = 0; j < n; ++j) buf[j] = value ? value + (int32_t)j * delta : 0;
  }
}

void AxWiiHle::add_to_lr(uint32_t addr, bool neg) {
  for (int i = 0; i < 96; ++i) {
    int32_t v = (int32_t)m_mem.rd32(addr + 4 * i);
    if (neg) v = -v;
    m_main[MAIN_L][i] += v;
    m_main[MAIN_R][i] += v;
  }
}

void AxWiiHle::add_sub_to_lr(uint32_t addr) {
  for (int i = 0; i < 96; ++i) m_main[MAIN_L][i] += (int32_t)m_mem.rd32(addr + 4 * i);
  for (int i = 0; i < 96; ++i) m_main[MAIN_R][i] -= (int32_t)m_mem.rd32(addr + 384 + 4 * i);
}

void AxWiiHle::process_pb_list(uint32_t addr) {
  PB pb;
  int guard = 0;
  while (addr && guard++ < 512) {
    read_pb(m_mem, addr, pb);
    if (pb.w[pbw::STATE] == 1) m_stats.last_voices++;
    process_voice(pb);
    write_pb(m_mem, addr, pb);
    addr = hilo(pb.w[pbw::NEXT_HI], pb.w[pbw::NEXT_LO]);
  }
  m_stats.voices += m_stats.last_voices;
  if (m_stats.last_voices > m_stats.max_voices) m_stats.max_voices = m_stats.last_voices;
}

void AxWiiHle::process_voice(PB &pb) {
  if (pb.w[pbw::STATE] != 1) return;
  constexpr uint32_t N = 96;
  int16_t s[N];

  // Input samples + sample-rate conversion (AXVoice.h GetInputSamples).
  Accelerator acc;
  acc.setup(m_mem, pb);
  const int16_t *coefs = polyphase_table() + (pb.w[pbw::COEF_SELECT] & 3) * 0x200;
  const uint32_t pos = resample([&] { return acc.read_sample(); }, s, N, &pb.w[pbw::SRC_LAST],
                                pb.w[pbw::SRC_FRAC], hilo(pb.w[pbw::SRC_RATIO_HI], pb.w[pbw::SRC_RATIO_LO]),
                                pb.w[pbw::SRC_SELECT], coefs);
  pb.w[pbw::SRC_FRAC] = (uint16_t)pos;
  acc.store(pb);

  // Volume envelope (u16 volume, s16 delta per sample).
  uint16_t ve = pb.w[pbw::VE_VOL];
  const uint16_t ve_delta = pb.w[pbw::VE_DELTA];
  for (uint32_t i = 0; i < N; ++i) {
    s[i] = clamp16(((int32_t)s[i] * (int32_t)ve) >> 15);
    ve = (uint16_t)(ve + ve_delta);
  }
  pb.w[pbw::VE_VOL] = ve;

  if (pb.w[pbw::LPF_ON]) lowpass(s, N, &pb.w[pbw::LPF_ON]);
  if (pb.w[pbw::BQ_ON]) biquad(s, N, &pb.w[pbw::BQ_ON]);

  // Mixing (mixer-control bits from AXPB.h AXMixerCtrlFlags).
  const uint32_t mc = hilo(pb.w[pbw::MIXER_HI], pb.w[pbw::MIXER_LO]);
  struct Route { int buf; uint32_t on, ramp; int vol, dpop; };
  static const Route routes[12] = {
      {MAIN_L, 1u << 0, 1u << 2, 0, 0},    {MAIN_R, 1u << 1, 1u << 2, 2, 4},
      {MAIN_S, 1u << 3, 1u << 4, 16, 8},   {A_L, 1u << 16, 1u << 18, 4, 1},
      {A_R, 1u << 17, 1u << 18, 6, 5},     {A_S, 1u << 19, 1u << 20, 18, 9},
      {B_L, 1u << 21, 1u << 23, 8, 2},     {B_R, 1u << 22, 1u << 23, 10, 6},
      {B_S, 1u << 24, 1u << 25, 20, 10},   {C_L, 1u << 26, 1u << 28, 12, 3},
      {C_R, 1u << 27, 1u << 28, 14, 7},    {C_S, 1u << 29, 1u << 30, 22, 11},
  };
  for (const Route &r : routes)
    if (mc & r.on)
      mix_add<N>(m_main[r.buf].data(), s, pb.w[pbw::MIX + r.vol], pb.w[pbw::MIX + r.vol + 1],
                 pb.w[pbw::DPOP + r.dpop], (mc & r.ramp) != 0);

  // Wii Remote speaker buses: 96 -> 18 samples (ratio 0x55555), 8 buses.
  if (pb.w[pbw::REMOTE]) {
    ++wm_activity().remote_voice_frames;
    uint16_t *iir = &pb.w[pbw::RMT_IIR];
    if (iir[0] == 2) biquad(s, N, iir);
    else if (iir[0] != 0) lowpass(s, N, iir);
    int16_t wm[18];
    uint32_t k = 0;
    const uint32_t rpos = resample([&] { return k < N ? s[k++] : s[N - 1]; }, wm, 18, &pb.w[pbw::RMT_SRC_LAST],
                                   pb.w[pbw::RMT_SRC_FRAC], 0x55555, SRC_POLYPHASE, polyphase_table());
    pb.w[pbw::RMT_SRC_FRAC] = (uint16_t)rpos;
    const uint16_t rmc = pb.w[pbw::RMT_MIXER];
    for (int ch = 0; ch < 8; ++ch) {  // main0 aux0 main1 aux1 ...
      if (!(rmc & (1u << (2 * ch)))) continue;
      const int dpop = (ch & 1) ? 4 + ch / 2 : ch / 2;
      mix_add<18>(m_wm[ch].data(), wm, pb.w[pbw::RMT_MIX + 2 * ch], pb.w[pbw::RMT_MIX + 2 * ch + 1],
                  pb.w[pbw::RMT_DPOP + dpop], (rmc & (1u << (2 * ch + 1))) != 0);
    }
  }
}

void AxWiiHle::mix_aux(int aux, uint32_t write_addr, uint32_t read_addr, uint16_t volume) {
  uint16_t ramp[96];
  volume_ramp(ramp, m_last_aux_vol[aux], volume, 96);
  m_last_aux_vol[aux] = volume;
  const int base = A_L + 3 * aux;
  if (write_addr)
    for (int c = 0; c < 3; ++c)
      for (int i = 0; i < 96; ++i) m_mem.wr32(write_addr + c * 384 + 4 * i, (uint32_t)m_main[base + c][i]);
  for (int c = 0; c < 3; ++c)
    for (int i = 0; i < 96; ++i) {
      const int64_t v = (int64_t)(int32_t)m_mem.rd32(read_addr + c * 384 + 4 * i) * ramp[i];
      m_main[MAIN_L + c][i] += (int32_t)(v >> 15);
    }
}

void AxWiiHle::upload_aux_mix_lrsc(int aux, const uint32_t *a, uint16_t volume) {
  const int base = aux ? B_L : A_L;
  for (int c = 0; c < 3; ++c)
    for (int i = 0; i < 96; ++i) m_mem.wr32(a[0] + c * 384 + 4 * i, (uint32_t)m_main[base + c][i]);
  const auto &auxc = m_main[aux ? C_S : C_R];
  for (int i = 0; i < 96; ++i) m_mem.wr32(a[1] + 4 * i, (uint32_t)auxc[i]);
  uint16_t ramp[96];
  volume_ramp(ramp, m_last_aux_vol[aux], volume, 96);
  m_last_aux_vol[aux] = volume;
  const int dest[4] = {MAIN_L, MAIN_R, MAIN_S, C_L};
  for (int d = 0; d < 4; ++d)
    for (int i = 0; i < 96; ++i) {
      const int64_t v = (int64_t)(int32_t)m_mem.rd32(a[2 + d] + 4 * i) * ramp[i];
      m_main[dest[d]][i] += (int32_t)(v >> 15);
    }
}

void AxWiiHle::compressor(uint16_t threshold, uint16_t release_frames, uint32_t table_addr) {
  constexpr uint32_t N = 96, frame_bytes = N * 2, ATTACK_ENTRIES = 11;
  bool over = false;
  for (uint32_t i = 0; i < N && !over; ++i)
    over = std::abs(m_main[MAIN_L][i]) > (int)threshold || std::abs(m_main[MAIN_R][i]) > (int)threshold;
  uint32_t off;
  if (over) {
    off = m_compressor_pos * frame_bytes;
    m_compressor_pos = release_frames;
  } else if (m_compressor_pos) {
    --m_compressor_pos;
    off = (ATTACK_ENTRIES + m_compressor_pos) * frame_bytes;
  } else {
    return;
  }
  for (uint32_t i = 0; i < N; ++i) {
    const int64_t c = m_mem.rd16(table_addr + off + 2 * i);
    m_main[MAIN_L][i] = (int32_t)(((int64_t)m_main[MAIN_L][i] * c) >> 15);
    m_main[MAIN_R][i] = (int32_t)(((int64_t)m_main[MAIN_R][i] * c) >> 15);
  }
}

void AxWiiHle::output(uint32_t lr_addr, uint32_t surround_addr, uint16_t volume, bool dpl2, std::vector<Mail> &out) {
  uint16_t ramp[96];
  volume_ramp(ramp, m_last_main_vol, volume, 96);
  m_last_main_vol = volume;
  for (int i = 0; i < 96; ++i) m_mem.wr32(surround_addr + 4 * i, (uint32_t)m_main[MAIN_S][i]);
  if (dpl2)
    for (int i = 0; i < 96; ++i) m_mem.wr32(surround_addr + 384 + 4 * i, (uint32_t)m_main[C_L][i]);
  for (int i = 0; i < 96; ++i) {
    const int16_t l = clamp16(((int64_t)m_main[MAIN_L][i] * ramp[i]) >> 15);
    const int16_t r = clamp16(((int64_t)m_main[MAIN_R][i] * ramp[i]) >> 15);
    m_main[MAIN_L][i] = l;
    m_main[MAIN_R][i] = r;
    // AI DMA stream order is R, L (Dolphin AXWii OutputSamples / Mixer).
    m_mem.wr16(lr_addr + 4 * i, (uint16_t)r);
    m_mem.wr16(lr_addr + 4 * i + 2, (uint16_t)l);
  }
  m_stats.outputs++;
  {
    const uint32_t a = lr_addr & 0x3FFFFFE0u;
    if (!m_stats.is_output_addr(a)) m_stats.out_addr[m_stats.out_addr_n++ & 3] = a;
  }
  out.push_back({DSP_SYNC, true});
}

void AxWiiHle::wm_output(const uint32_t *a) {
  static const int ch[4] = {0, 2, 4, 6};  // main buses of remotes 0..3
  for (int r = 0; r < 4; ++r)
    for (int j = 0; j < 18; ++j) m_mem.wr16(a[r] + 2 * j, (uint16_t)clamp16(m_wm[ch[r]][j]));
  WmActivity &w = wm_activity();
  if (!w.on) return;
  ++w.frames;
  for (int r = 0; r < 4; ++r) {
    bool nz = false;
    for (int j = 0; j < 18; ++j) {
      const int v = std::abs((int)clamp16(m_wm[ch[r]][j]));
      nz = nz || v != 0;
      if (v > w.peak[r]) w.peak[r] = v;
    }
    w.nonzero[r] += nz;
  }
  if (w.frames % 3333 == 0)
    std::fprintf(stderr, "[SPK] AX remote buses: frames=%llu remote-voice-frames=%llu (+%llu) nonzero-frames r0..3=%llu/%llu/%llu/%llu peak r0=%d\n",
                 (unsigned long long)w.frames, (unsigned long long)w.remote_voice_frames,
                 (unsigned long long)(w.remote_voice_frames - w.last_remote_voice_frames),
                 (unsigned long long)w.nonzero[0], (unsigned long long)w.nonzero[1], (unsigned long long)w.nonzero[2],
                 (unsigned long long)w.nonzero[3], w.peak[0]);
  if (w.frames % 3333 == 0) w.last_remote_voice_frames = w.remote_voice_frames;
}

}  // namespace nwii::runtime::dsp
