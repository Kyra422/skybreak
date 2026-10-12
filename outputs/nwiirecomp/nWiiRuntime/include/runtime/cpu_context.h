#pragma once
#include <array>
#include <cmath>
#include <chrono>
#include <csetjmp>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>
#include <mutex>
#include <atomic>
#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h> // __umulh (tb_from_inst)
#endif
#include <queue>
#include <stack>
#include <stdexcept>
#include "runtime/gekko_fp.h" // Gekko FP semantics (FP track)

namespace nwii {
namespace runtime {

struct ConditionField {
  bool lt; 
  bool gt; 
  bool eq; 
  bool so; 

  ConditionField() : lt(false), gt(false), eq(false), so(false) {}
};

void GX_WGPIPE_Write8(uint8_t val);
void GX_WGPIPE_Write16(uint16_t val);
void GX_WGPIPE_Write32(uint32_t val);
void GX_DL_Snapshot(uint32_t addr, uint32_t size);
bool GX_DL_Fetch(uint32_t addr, uint32_t size, std::vector<uint8_t>& out);
void GX_WGPIPE_WriteF32(float val);
void GX_WGPIPE_WriteF64(double val);

uint16_t HW_Reg_Read16(uint32_t addr);
uint32_t HW_Reg_Read32(uint32_t addr);
void HW_Reg_Write16(uint32_t addr, uint16_t val);
void HW_Reg_Write32(uint32_t addr, uint32_t val);

struct CPUContext;
void init_ipc_client(CPUContext &ctx);
bool handle_syscall(CPUContext &ctx);
bool process_pending_callbacks(CPUContext &ctx);
// Number of process_pending_callbacks calls so far (PERF2 profiler; racy read).
uint64_t poll_call_count();

// FP Unavailable emulation (see CPUContext::fp_unavailable). Generated code
// that carries the MSR[FP] guards sets this at static initialisation; the REL
// interpreter applies the same guards only then, so translated and
// interpreted code always follow one FPU ownership model.
inline bool g_fp_unavailable_emulation = false;
// NWII_FP_UNAVAILABLE=0 restores the old shared-FPR behaviour (negative control).
inline const bool g_env_fp_unavailable_off = [] {
  const char *e = std::getenv("NWII_FP_UNAVAILABLE");
  return e && e[0] == '0';
}();
// FP track diagnostic (src/core/fp_trap.cpp): NWII_FPTRAP=<max events> traps
// host invalid operations (NaN creation) on the CPU thread. Off by default.
inline const int g_env_fptrap = [] {
  const char *e = std::getenv("NWII_FPTRAP");
  return e ? (std::atoi(e) > 0 ? std::atoi(e) : 1) : 0;
}();
// Hooks installed by fp_trap.cpp at start-up (function pointers, so code that only
// includes this header links without fp_trap.cpp).
inline void (*g_fptrap_arm_hook)() = nullptr;
// NWII_NANSTORE=1: report float stores (stfs/stfd/psq_st) of NaN values, first per guest function.
inline const bool g_env_nanstore = std::getenv("NWII_NANSTORE") != nullptr;
inline void (*g_nanstore_hook)(uint32_t addr, uint32_t bits) = nullptr;
// PEEK10 (roadmap M3.6): CPU EFB access window, physical 0x08000000-0x0BFFFFFF
// (Dolphin MMU EFB_Read/EFB_Write; GXPeekZ/GXPeekARGB/GXPoke*). Installed by
// hw_pe.cpp; null = reads return 0 and writes are dropped (as before). Only the
// *_slow paths reach it: the fastmem windows never cover 0x88../0xC8...
inline uint32_t (*g_efb_read_hook)(uint32_t paddr) = nullptr;
inline void (*g_efb_write_hook)(uint32_t paddr, uint32_t value) = nullptr;
inline bool is_efb_paddr(uint32_t paddr) { return (paddr & 0xFC000000u) == 0x08000000u; }

struct CallbackInfo {
    uint32_t cb_addr;
    uint32_t arg1;
    uint32_t arg2;
    bool is_irq;
};



void watch_hit(uint32_t addr, uint32_t value, int width);
extern uint32_t g_watch_addr;

// Environment switches consulted by the inline helpers below, some of which
// run on hot paths (read_timebase on every poll and mftb, write_dec, locked-
// cache DMA). Namespace-scope inline variables are initialised once during
// program start-up, so each use is a plain load: no std::getenv per call and
// no function-local static (MSVC checks a thread-local init epoch on every
// call of a function with a dynamically initialised static).
inline const bool g_env_walltb = std::getenv("NWII_WALLTB") != nullptr;
inline const bool g_env_sample = std::getenv("NWII_SAMPLE") != nullptr;
inline const bool g_env_lc_trace = std::getenv("NWII_LC_TRACE") != nullptr;

// PERF2 / roadmap M2.6: guest instructions per timebase tick. Default 12,
// Dolphin's ratio: Broadway's timebase ticks at 1/12 of the core clock
// (Dolphin SystemTimers TIMER_RATIO) and Dolphin counts about one cycle per
// instruction. NWII_TIMING=legacy restores the historical clock (k = 1, TB =
// inst_count, under which the guest saw a CPU 12x slower than Broadway);
// NWII_TB_DIV=<k> overrides. tb = inst / k is computed with a multiply-high
// by ceil(2^64/k), exact for inst <= 2^64/k (k <= 64). The idle skip and
// real-time throttle that go with it live in ios.cpp (on_guest_idle).
inline const uint64_t g_tb_div = [] {
  uint64_t k = 12;
  if (const char *t = std::getenv("NWII_TIMING"))
    if (std::strcmp(t, "legacy") == 0) k = 1;
  if (const char *e = std::getenv("NWII_TB_DIV")) {
    const long v = std::atol(e);
    if (v >= 1 && v <= 64) k = (uint64_t)v;
  }
  return k;
}();
inline const uint64_t g_tb_magic = g_tb_div > 1 ? UINT64_MAX / g_tb_div + 1 : 0;
inline uint64_t tb_from_inst(uint64_t inst) {
  if (g_tb_div == 1) return inst;
#if defined(_MSC_VER) && defined(_M_X64)
  return __umulh(inst, g_tb_magic);
#else
  return (uint64_t)(((unsigned __int128)inst * g_tb_magic) >> 64);
#endif
}

// PERF3 fastmem (roadmap M2.9): force-inline / no-inline markers and byte swaps
// for the MMU helpers below.
#if defined(_MSC_VER)
#include <stdlib.h>
// The generated code is built with /O1, which does not imply /Oi: without
// this pragma _byteswap_* are CRT calls instead of a bswap instruction.
#pragma intrinsic(_byteswap_ushort, _byteswap_ulong, _byteswap_uint64)
// Same for memcpy: the fixed-size copies below must become plain moves.
#pragma intrinsic(memcpy)
#define NWII_FORCEINLINE __forceinline
#define NWII_NOINLINE __declspec(noinline)
#else
#define NWII_FORCEINLINE inline __attribute__((always_inline))
#define NWII_NOINLINE __attribute__((noinline))
#endif
NWII_FORCEINLINE uint16_t mmu_bswap16(uint16_t v) {
#if defined(_MSC_VER)
  return _byteswap_ushort(v);
#else
  return __builtin_bswap16(v);
#endif
}
NWII_FORCEINLINE uint32_t mmu_bswap32(uint32_t v) {
#if defined(_MSC_VER)
  return _byteswap_ulong(v);
#else
  return __builtin_bswap32(v);
#endif
}
NWII_FORCEINLINE uint64_t mmu_bswap64(uint64_t v) {
#if defined(_MSC_VER)
  return _byteswap_uint64(v);
#else
  return __builtin_bswap64(v);
#endif
}

// NWII_FASTMEM=0: every MMU access takes the *_slow body (A/B, diagnosis).
// Masks the fast-window bounds (no extra branch); zero before dynamic
// initialisation, so accesses from earlier static initialisers are slow-path.
inline const size_t g_fastmem_mask = [] {
  const char *e = std::getenv("NWII_FASTMEM");
  return (e && e[0] == '0') ? size_t{0} : ~size_t{0};
}();

// Strict Dolphin-accurate MMU
struct MMU {
  std::vector<uint8_t> mem1;
  std::vector<uint8_t> mem2;
  static constexpr uint32_t locked_cache_base = 0xE0000000;
  static constexpr uint32_t locked_cache_size = 16 * 1024;
  std::array<uint8_t, locked_cache_size> locked_cache{};

  MMU() {
    mem1.resize(24 * 1024 * 1024 + 8, 0); 
    mem2.resize(64 * 1024 * 1024 + 8, 0); 
  }

  inline uint8_t *get_ptr(uint32_t addr, uint32_t size = 1) {
    // The SDK allocates all 512 locked-cache lines at this effective address.
    // Check the whole access before exposing the host array, including scalar
    // reads which straddle its end. This region must not alias ordinary RAM.
    if (addr >= locked_cache_base) {
      const uint32_t offset = addr - locked_cache_base;
      if (offset < locked_cache_size && size <= locked_cache_size - offset)
        return locked_cache.data() + offset;
      return nullptr;
    }
    uint32_t paddr = addr & 0x3FFFFFFF; 
    if (paddr < mem1.size() - 8 && size <= mem1.size() - 8 - paddr) {
        return &mem1[paddr];
    } else if (paddr >= 0x10000000 && paddr < 0x10000000 + mem2.size() - 8 &&
               size <= mem2.size() - 8 - (paddr - 0x10000000)) {
        return &mem2[paddr - 0x10000000];
    }
    return nullptr;
  }

  void zero_cache_line(uint32_t addr) {
    uint8_t* ptr = get_ptr(addr & ~31u, 32);
    if (!ptr)
      throw std::runtime_error("dcbz address outside mapped RAM/locked cache");
    std::memset(ptr, 0, 32);
  }

  void transfer_locked_cache(uint32_t memory_addr, uint32_t cache_addr,
                             uint32_t size, bool load) {
    // Gekko User's Manual 2.1.2.8 and 9.3: both addresses and the transfer
    // length use 32-byte cache lines; validate both ends before any mutation.
    if ((memory_addr & 31) || (cache_addr & 31) || !size || (size & 31) ||
        size > 4096 || cache_addr < locked_cache_base ||
        cache_addr - locked_cache_base >= locked_cache_size ||
        size > locked_cache_size - (cache_addr - locked_cache_base) ||
        memory_addr >= 0x20000000)
      throw std::runtime_error("Invalid locked-cache DMA address/length");
    uint8_t* ram = get_ptr(memory_addr, size);
    uint8_t* cache = get_ptr(cache_addr, size);
    if (!ram || !cache)
      throw std::runtime_error("Locked-cache DMA outside mapped MEM1/MEM2");
    if (load) std::memcpy(cache, ram, size);
    else std::memcpy(ram, cache, size);
  }

  inline bool is_hw_reg(uint32_t paddr) const {
      return (paddr >= 0x0C000000 && paddr < 0x0C010000) || 
             (paddr >= 0x0D000000 && paddr < 0x0D010000);
  }

  NWII_NOINLINE uint8_t read8_slow(uint32_t addr) {
    uint32_t paddr = addr & 0x3FFFFFFF;

    if (is_hw_reg(paddr)) {
        
        uint32_t val = HW_Reg_Read32(paddr & ~3);
        int shift = 24 - ((paddr & 3) * 8);
        return (val >> shift) & 0xFF;
    }
    if (is_efb_paddr(paddr) && g_efb_read_hook) return (uint8_t)g_efb_read_hook(paddr);
    uint8_t *ptr = get_ptr(addr);
    return ptr ? *ptr : 0;
  }

  void custom_watch(uint32_t, uint32_t, int) {}

  NWII_NOINLINE void write8_slow(uint32_t addr, uint8_t value) {
    custom_watch(addr, value, 8);
    uint32_t paddr = addr & 0x3FFFFFFF;
    if (g_watch_addr && (paddr & ~3u) == (g_watch_addr & 0x3FFFFFFC))
      watch_hit(addr, value, 8);

    // The whole 0x0C008xxx page mirrors the write-gather pipe (Dolphin

    
    if ((paddr & 0xFFFFF000u) == 0x0C008000) { GX_WGPIPE_Write8(value); return; } 
    if (is_hw_reg(paddr)) return; 
    if (is_efb_paddr(paddr) && g_efb_write_hook) { g_efb_write_hook(paddr, value); return; }

    uint8_t *ptr = get_ptr(addr);
    if (ptr) *ptr = value;
  }

  NWII_NOINLINE uint16_t read16_slow(uint32_t addr) {
    uint32_t paddr = addr & 0x3FFFFFFF;

    if (is_hw_reg(paddr)) return HW_Reg_Read16(paddr);
    if (is_efb_paddr(paddr) && g_efb_read_hook) return (uint16_t)g_efb_read_hook(paddr);
    uint8_t *ptr = get_ptr(addr, 2);
    if (ptr) return (ptr[0] << 8) | ptr[1];
    return 0;
  }

  NWII_NOINLINE void write16_slow(uint32_t addr, uint16_t value) {
    custom_watch(addr, value, 16);
    uint32_t paddr = addr & 0x3FFFFFFF;
    if (g_watch_addr && (paddr & ~3u) == (g_watch_addr & 0x3FFFFFFC))
      watch_hit(addr, value, 16);

    if ((paddr & 0xFFFFF000u) == 0x0C008000) { GX_WGPIPE_Write16(value); return; } 
    if (is_hw_reg(paddr)) { HW_Reg_Write16(paddr, value); return; }
    if (is_efb_paddr(paddr) && g_efb_write_hook) { g_efb_write_hook(paddr, value); return; }

    uint8_t *ptr = get_ptr(addr, 2);
    if (ptr) { ptr[0] = value >> 8; ptr[1] = value & 0xFF; }
  }

  NWII_NOINLINE uint32_t read32_slow(uint32_t addr) {
    uint32_t paddr = addr & 0x3FFFFFFF;
    if (paddr == 0x0000000C) return 0; 

    if (is_hw_reg(paddr)) return HW_Reg_Read32(paddr);
    if (is_efb_paddr(paddr) && g_efb_read_hook) return g_efb_read_hook(paddr);

    uint8_t *ptr = get_ptr(addr, 4);
    if (!ptr) return 0;
    uint32_t val = ((uint32_t)ptr[0] << 24) | ((uint32_t)ptr[1] << 16) | ((uint32_t)ptr[2] << 8) | ptr[3];

    return val;
  }

  NWII_NOINLINE void write32_slow(uint32_t addr, uint32_t value) {
    custom_watch(addr, value, 32);
    uint32_t paddr = addr & 0x3FFFFFFF;
    if (paddr == 0x0000000C) return; 
    if (g_watch_addr && (paddr & ~3u) == (g_watch_addr & 0x3FFFFFFC))
      watch_hit(addr, value, 32);

    if ((paddr & 0xFFFFF000u) == 0x0C008000) { GX_WGPIPE_Write32(value); return; }
    if (is_hw_reg(paddr)) { HW_Reg_Write32(paddr, value); return; }
    if (is_efb_paddr(paddr) && g_efb_write_hook) { g_efb_write_hook(paddr, value); return; }

    uint8_t *ptr = get_ptr(addr, 4);
    if (ptr) { ptr[0] = value >> 24; ptr[1] = (value >> 16) & 0xFF; ptr[2] = (value >> 8) & 0xFF; ptr[3] = value & 0xFF; }
  }

  NWII_NOINLINE void write_f32_slow(uint32_t addr, float value) {
    uint32_t paddr = addr & 0x3FFFFFFF;
    
    uint32_t v;
    std::memcpy(&v, &value, sizeof(float));
    if (g_watch_addr && (paddr & ~3u) == (g_watch_addr & 0x3FFFFFFC))
      watch_hit(addr, v, 32);
    if (g_env_nanstore && (v & 0x7F800000u) == 0x7F800000u && (v & 0x007FFFFFu))
      if (g_nanstore_hook) g_nanstore_hook(addr, v); // FP diagnostic (fp_trap.cpp)

    if ((paddr & 0xFFFFF000u) == 0x0C008000) { GX_WGPIPE_WriteF32(value); return; } 

    uint8_t *ptr = get_ptr(addr, 4);
    if (ptr) {
      ptr[0] = v >> 24; ptr[1] = (v >> 16) & 0xFF; ptr[2] = (v >> 8) & 0xFF; ptr[3] = v & 0xFF;
    }
  }

  NWII_NOINLINE void write_f64_slow(uint32_t addr, double value) {
    uint32_t paddr = addr & 0x3FFFFFFF;
    
    uint64_t v;
    std::memcpy(&v, &value, sizeof(double));
    if (g_watch_addr && (paddr & ~7u) == (g_watch_addr & 0x3FFFFFF8))
      watch_hit(addr, v >> 32, 64);
    if (g_env_nanstore && (v & 0x7FF0000000000000ull) == 0x7FF0000000000000ull && (v & 0x000FFFFFFFFFFFFFull))
      if (g_nanstore_hook) g_nanstore_hook(addr, uint32_t(v >> 32)); // FP diagnostic (fp_trap.cpp)

    if ((paddr & 0xFFFFF000u) == 0x0C008000) { GX_WGPIPE_WriteF64(value); return; } 

    uint8_t *ptr = get_ptr(addr, 8);
    if (ptr) {
      ptr[0] = v >> 56; ptr[1] = (v >> 48) & 0xFF; ptr[2] = (v >> 40) & 0xFF; ptr[3] = (v >> 32) & 0xFF;
      ptr[4] = (v >> 24) & 0xFF; ptr[5] = (v >> 16) & 0xFF; ptr[6] = (v >> 8) & 0xFF; ptr[7] = v & 0xFF;
    }
  }

  NWII_NOINLINE uint64_t read64_slow(uint32_t addr) {
    uint8_t *ptr = get_ptr(addr, 8);
    if (!ptr) return 0;
    return ((uint64_t)ptr[0] << 56) | ((uint64_t)ptr[1] << 48) | ((uint64_t)ptr[2] << 40) | ((uint64_t)ptr[3] << 32) |
           ((uint64_t)ptr[4] << 24) | ((uint64_t)ptr[5] << 16) | ((uint64_t)ptr[6] << 8) | ptr[7];
  }
  NWII_NOINLINE void write64_slow(uint32_t addr, uint64_t value) {
    uint32_t paddr = addr & 0x3FFFFFFF;
    if (g_watch_addr && (paddr & ~7u) == (g_watch_addr & 0x3FFFFFF8))
      watch_hit(addr, value >> 32, 64);

    uint8_t *ptr = get_ptr(addr, 8);
    if (!ptr) return;
    ptr[0] = value >> 56; ptr[1] = (value >> 48) & 0xFF; ptr[2] = (value >> 40) & 0xFF; ptr[3] = (value >> 32) & 0xFF;
    ptr[4] = (value >> 24) & 0xFF; ptr[5] = (value >> 16) & 0xFF; ptr[6] = (value >> 8) & 0xFF; ptr[7] = value & 0xFF;
  }
  NWII_NOINLINE double read_f64_slow(uint32_t addr) {
      uint8_t *ptr = get_ptr(addr, 8);
      if (ptr) {
          uint64_t val = ((uint64_t)ptr[0] << 56) | ((uint64_t)ptr[1] << 48) | ((uint64_t)ptr[2] << 40) | ((uint64_t)ptr[3] << 32) |
                         ((uint64_t)ptr[4] << 24) | ((uint64_t)ptr[5] << 16) | ((uint64_t)ptr[6] << 8) | ptr[7];
          double f; std::memcpy(&f, &val, 8); return f;
      }
      return 0.0;
  }

  // ---- PERF3 fastmem (roadmap M2.9) --------------------------------------
  // The generated code calls these for every guest load/store. The *_slow
  // bodies above are the reference semantics (MMIO, write-gather pipe,
  // locked cache, the paddr 0xC special case, watch/NaN-store diagnostics,
  // unmapped -> 0 / dropped). The wrappers below only short-cut accesses that
  // those bodies would serve from ordinary RAM through get_ptr, with the same
  // bounds (mem1/mem2.size() - 8, so harnesses that shrink a vector keep
  // their bounds), and fall back to *_slow for everything else:
  //   MEM1 cached mirror 0x80000010.. (0x80000000-0x8000000F excluded: the
  //   read32/write32 bodies special-case physical 0xC), MEM2 cached mirror
  //   0x90000000.. . Uncached mirrors, MMIO, the locked cache and unmapped
  //   addresses take the slow path. Writes take it whenever a diagnostic
  //   watch (g_watch_addr) or NWII_NANSTORE is active.
  // Big-endian access via memcpy + bswap intrinsics (one mov + bswap).
  // Test: outputs/pc-tests/perf3/fastmem-test.cpp (differential vs *_slow).
  static constexpr uint32_t kFastMem1Lo = 0x10;
  // NWII_FASTMEM=0 (A/B, diagnosis): the fast windows become empty, so every
  // access takes the *_slow body. Applied as a mask on the bound, not as an
  // extra branch (see g_fastmem_mask).
  // Offset of addr in the MEM1 / MEM2 fast window, and whether an N-byte
  // access there is RAM (exactly get_ptr's bound: offset + N <= size - 8).
  template <uint32_t N>
  NWII_FORCEINLINE bool m1_fast(uint32_t addr) const {
    return uint64_t{addr - (0x80000000u + kFastMem1Lo)} + N <=
           ((mem1.size() - (8 + kFastMem1Lo)) & g_fastmem_mask);
  }
  template <uint32_t N>
  NWII_FORCEINLINE bool m2_fast(uint32_t addr) const {
    return uint64_t{addr - 0x90000000u} + N <= ((mem2.size() - 8) & g_fastmem_mask);
  }
  NWII_FORCEINLINE uint8_t *m1_at(uint32_t addr) {
    return mem1.data() + kFastMem1Lo + (addr - (0x80000000u + kFastMem1Lo));
  }
  NWII_FORCEINLINE uint8_t *m2_at(uint32_t addr) { return mem2.data() + (addr - 0x90000000u); }
  // The RAM byte pointer the wrappers would use, or nullptr (tests, tools).
  template <uint32_t N>
  NWII_FORCEINLINE uint8_t *ram_ptr(uint32_t addr) {
    if (m1_fast<N>(addr)) return m1_at(addr);
    if (m2_fast<N>(addr)) return m2_at(addr);
    return nullptr;
  }
  static NWII_FORCEINLINE uint16_t ld16(const uint8_t *p) { uint16_t v; std::memcpy(&v, p, 2); return mmu_bswap16(v); }
  static NWII_FORCEINLINE uint32_t ld32(const uint8_t *p) { uint32_t v; std::memcpy(&v, p, 4); return mmu_bswap32(v); }
  static NWII_FORCEINLINE uint64_t ld64(const uint8_t *p) { uint64_t v; std::memcpy(&v, p, 8); return mmu_bswap64(v); }
  static NWII_FORCEINLINE void st16(uint8_t *p, uint16_t v) { v = mmu_bswap16(v); std::memcpy(p, &v, 2); }
  static NWII_FORCEINLINE void st32(uint8_t *p, uint32_t v) { v = mmu_bswap32(v); std::memcpy(p, &v, 4); }
  static NWII_FORCEINLINE void st64(uint8_t *p, uint64_t v) { v = mmu_bswap64(v); std::memcpy(p, &v, 8); }
  static NWII_FORCEINLINE uint32_t f32_bits(float f) { uint32_t v; std::memcpy(&v, &f, 4); return v; }
  static NWII_FORCEINLINE uint64_t f64_bits(double d) { uint64_t v; std::memcpy(&v, &d, 8); return v; }

  NWII_FORCEINLINE uint8_t read8(uint32_t addr) {
    if (m1_fast<1>(addr)) return *m1_at(addr);
    if (m2_fast<1>(addr)) return *m2_at(addr);
    return read8_slow(addr);
  }
  NWII_FORCEINLINE uint16_t read16(uint32_t addr) {
    if (m1_fast<2>(addr)) return ld16(m1_at(addr));
    if (m2_fast<2>(addr)) return ld16(m2_at(addr));
    return read16_slow(addr);
  }
  NWII_FORCEINLINE uint32_t read32(uint32_t addr) {
    if (m1_fast<4>(addr)) return ld32(m1_at(addr));
    if (m2_fast<4>(addr)) return ld32(m2_at(addr));
    return read32_slow(addr);
  }
  NWII_FORCEINLINE float read_f32(uint32_t addr) {
    const uint32_t v = read32(addr);
    float f; std::memcpy(&f, &v, 4); return f;
  }
  NWII_FORCEINLINE uint64_t read64(uint32_t addr) {
    if (m1_fast<8>(addr)) return ld64(m1_at(addr));
    if (m2_fast<8>(addr)) return ld64(m2_at(addr));
    return read64_slow(addr);
  }
  NWII_FORCEINLINE double read_f64(uint32_t addr) {
    uint64_t v;
    if (m1_fast<8>(addr)) v = ld64(m1_at(addr));
    else if (m2_fast<8>(addr)) v = ld64(m2_at(addr));
    else return read_f64_slow(addr);
    double f; std::memcpy(&f, &v, 8); return f;
  }
  NWII_FORCEINLINE void write8(uint32_t addr, uint8_t value) {
    if (!g_watch_addr) {
      if (m1_fast<1>(addr)) { *m1_at(addr) = value; return; }
      if (m2_fast<1>(addr)) { *m2_at(addr) = value; return; }
    }
    write8_slow(addr, value);
  }
  NWII_FORCEINLINE void write16(uint32_t addr, uint16_t value) {
    if (!g_watch_addr) {
      if (m1_fast<2>(addr)) { st16(m1_at(addr), value); return; }
      if (m2_fast<2>(addr)) { st16(m2_at(addr), value); return; }
    }
    write16_slow(addr, value);
  }
  NWII_FORCEINLINE void write32(uint32_t addr, uint32_t value) {
    if (!g_watch_addr) {
      if (m1_fast<4>(addr)) { st32(m1_at(addr), value); return; }
      if (m2_fast<4>(addr)) { st32(m2_at(addr), value); return; }
    }
    write32_slow(addr, value);
  }
  NWII_FORCEINLINE void write_f32(uint32_t addr, float value) {
    if (!g_watch_addr && !g_env_nanstore) {
      if (m1_fast<4>(addr)) { st32(m1_at(addr), f32_bits(value)); return; }
      if (m2_fast<4>(addr)) { st32(m2_at(addr), f32_bits(value)); return; }
    }
    write_f32_slow(addr, value);
  }
  NWII_FORCEINLINE void write64(uint32_t addr, uint64_t value) {
    if (!g_watch_addr) {
      if (m1_fast<8>(addr)) { st64(m1_at(addr), value); return; }
      if (m2_fast<8>(addr)) { st64(m2_at(addr), value); return; }
    }
    write64_slow(addr, value);
  }
  NWII_FORCEINLINE void write_f64(uint32_t addr, double value) {
    if (!g_watch_addr && !g_env_nanstore) {
      if (m1_fast<8>(addr)) { st64(m1_at(addr), f64_bits(value)); return; }
      if (m2_fast<8>(addr)) { st64(m2_at(addr), f64_bits(value)); return; }
    }
    write_f64_slow(addr, value);
  }

  // Out-of-line forms of the same wrappers (fast path + slow fallback in one
  // small function). The generator emits these instead of the inline forms in
  // functions with very many memory accesses (recompiler.cpp, kOutlineMemSites):
  // MSVC's optimizer time grows superlinearly with the blocks of a single huge
  // generated function, and inlining every access there made a full rebuild
  // 2.6x slower while those functions are mostly one-shot initialisers.
  NWII_NOINLINE uint8_t read8_ol(uint32_t addr) { return read8(addr); }
  NWII_NOINLINE uint16_t read16_ol(uint32_t addr) { return read16(addr); }
  NWII_NOINLINE uint32_t read32_ol(uint32_t addr) { return read32(addr); }
  NWII_NOINLINE float read_f32_ol(uint32_t addr) { return read_f32(addr); }
  NWII_NOINLINE uint64_t read64_ol(uint32_t addr) { return read64(addr); }
  NWII_NOINLINE double read_f64_ol(uint32_t addr) { return read_f64(addr); }
  NWII_NOINLINE void write8_ol(uint32_t addr, uint8_t value) { write8(addr, value); }
  NWII_NOINLINE void write16_ol(uint32_t addr, uint16_t value) { write16(addr, value); }
  NWII_NOINLINE void write32_ol(uint32_t addr, uint32_t value) { write32(addr, value); }
  NWII_NOINLINE void write_f32_ol(uint32_t addr, float value) { write_f32(addr, value); }
  NWII_NOINLINE void write64_ol(uint32_t addr, uint64_t value) { write64(addr, value); }
  NWII_NOINLINE void write_f64_ol(uint32_t addr, double value) { write_f64(addr, value); }

};

struct CPUContext {
  
  std::array<uint32_t, 32> gpr;

  std::array<double, 32> fpr;

  std::array<double, 32> ps1;

  std::array<ConditionField, 8> cr;

  uint32_t pc;    
  uint32_t lr;    
  uint32_t ctr;   
  uint32_t xer;   
  uint32_t msr;   
  uint32_t fpscr; 
  uint32_t srr0;  
  uint32_t srr1;  

  std::array<uint32_t, 8> gqr;
  std::array<uint32_t, 4> sprg; 

  struct BackupState {
    std::array<uint32_t, 32> gpr;
    std::array<double, 32> fpr;
    std::array<double, 32> ps1;
    std::array<ConditionField, 8> cr;
    uint32_t lr;
    uint32_t ctr;
    uint32_t xer;
    uint32_t pc;
    uint32_t srr0;
    uint32_t srr1;
    uint32_t msr;
    uint32_t fpscr;
    std::array<uint32_t, 8> gqr;
    std::array<uint32_t, 4> sprg;
  };
  std::stack<BackupState> backup_stack;
  bool in_callback = false;
  int callback_depth = 0;

  MMU mmu;

  // The CPU owns these hardware registers across callbacks/thread switches.
  // DMA completes synchronously after a real copy, so DMAQL is always zero.
  uint32_t lc_hid2 = 0;
  uint32_t lc_dmau = 0;
  uint32_t lc_dmal = 0;
  uint64_t lc_dma_transfers = 0;
  uint64_t lc_dma_bytes = 0;

  uint32_t read_lc_spr(uint32_t spr) const {
    switch (spr) {
    case 920: return lc_hid2;
    case 922: return lc_dmau;
    case 923: return lc_dmal;
    default: throw std::runtime_error("Unsupported locked-cache SPR read");
    }
  }

  void write_lc_spr(uint32_t spr, uint32_t value) {
    if (spr == 920) {
      // DMAQL and reserved bits are read-only. Preserve graphics enable bits
      // as well as LC/error controls used by the original SDK initialization.
      lc_hid2 = value & 0xF0FF0000u;
      return;
    }
    if (spr == 922) { lc_dmau = value; return; }
    if (spr != 923)
      throw std::runtime_error("Unsupported locked-cache SPR write");
    lc_dmal = value;
    if (value & 1) {
      // Flush dominates trigger. The synchronous queue is already empty.
      lc_dmal &= ~3u;
      return;
    }
    if (!(value & 2)) return;
    const uint32_t memory_addr = lc_dmau & ~31u;
    const uint32_t cache_addr = value & ~31u;
    uint32_t blocks = ((lc_dmau & 31) << 2) | ((value >> 2) & 3);
    if (!blocks) blocks = 128;
    const uint32_t size = blocks * 32;
    const bool load = (value & 0x10) != 0;
    try {
      if (!(lc_hid2 & 0x10000000))
        throw std::runtime_error("Locked-cache DMA while HID2.LCE is disabled");
      mmu.transfer_locked_cache(memory_addr, cache_addr, size, load);
    } catch (...) {
      if (g_env_lc_trace)
        std::cerr << "[LC DMA error] pc=0x" << std::hex << pc << " memory=0x"
                  << memory_addr << " cache=0x" << cache_addr << std::dec
                  << " bytes=" << size << " load=" << load << '\n';
      throw;
    }
    lc_dmal &= ~2u;
    ++lc_dma_transfers;
    lc_dma_bytes += size;
    if (g_env_lc_trace &&
        (lc_dma_transfers <= 8 || lc_dma_transfers % 1000 == 0))
      std::cerr << "[LC DMA] count=" << lc_dma_transfers << " total_bytes="
                << lc_dma_bytes << " memory=0x" << std::hex << memory_addr
                << " cache=0x" << cache_addr << std::dec << " bytes=" << size
                << " load=" << load << '\n';
  }

  void zero_cache_line(uint32_t addr) { mmu.zero_cache_line(addr); }

  uint32_t exception_pc;
  uint64_t inst_count = 0;
  
  std::atomic<bool> is_running;
  std::atomic<bool> vblank_pending;
  std::mutex cb_mutex;
  std::queue<CallbackInfo> pending_callbacks;
  
  std::atomic<int> pending_cb_count{0};
  jmp_buf exception_jmp_buf;

  uint32_t reservation_addr = 0xFFFFFFFF;

  

  uint32_t dispatch_saved_ctx = 0;

  
  bool ext_resched_pending = false;

  



  

  
  uint64_t tb_freq = 40500000;
  std::chrono::steady_clock::time_point tb_start =
      std::chrono::steady_clock::now();
  // CLOCK15: time base value at boot = ticks since 2000-01-01 local time (Dolphin
  // CoreTiming FakeTBStartValue, see guest_clock.h); set once by main.cpp before
  // the CPU thread starts. 0 = the pre-CLOCK15 clock (NWII_RTC=0, test harnesses).
  uint64_t tb_base = 0;



  
  // Instruction count is a virtual clock: the guest gets a fixed number of
  // instructions per emulated frame and never falls behind. Real elapsed time
  // (NWII_WALLTB=1) is the hardware-accurate source, but while we run slower
  // than real time it starves the game — measured on MP7: same input script,
  // 122 DVD reads with the virtual clock vs 52 with the wall clock. Switch the
  // default once the drain is fast enough to keep up.
  uint64_t read_timebase() const {
    const bool wall = g_env_walltb;
    if (wall) {
      uint64_t us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - tb_start)
                        .count();
      return tb_base + us / 1000000 * tb_freq + us % 1000000 * tb_freq / 1000000; // CLOCK15: + tb_base
    }
    return tb_base + tb_from_inst(inst_count); // CLOCK15: + tb_base
  }
  // CLOCK15: the inst_count at which read_timebase() reaches `tb` (>= tb_base);
  // the idle skip advances the clock with it (tb_from_inst is exact on k * n).
  uint64_t inst_for_tb(uint64_t tb) const { return (tb - tb_base) * g_tb_div; }
  // CLOCK15: boot the clock at `base` (main.cpp, before the CPU thread starts).
  // The decrementer counts down from its reset value from the boot instant
  // (Dolphin PowerPC::Reset: DEC = 0xFFFFFFFF), not from time base 0 - else
  // mfdec before the first mtdec would read 0 with a 2026 time base.
  void start_clock(uint64_t base) {
    tb_base = base;
    dec_written_tb = read_timebase();
  }

  
  uint64_t dec_value = 0xFFFFFFFF;
  uint64_t dec_written_tb = 0;
  bool dec_irq_pending = false;

  void write_dec(uint32_t v) {
    if (g_env_sample) {
      static uint64_t n = 0;
      ++n;
      if (n <= 10 || (n % 500) == 0)
        std::cout << "[DEC] mtdec #" << n << " val=" << v << " pc=0x"
                  << std::hex << pc << " lr=0x" << lr << std::dec
                  << " inst=" << inst_count << "\n";
    }
    dec_value = v;
    dec_written_tb = read_timebase();
    dec_irq_pending = (v != 0xFFFFFFFF);
  }

  uint32_t read_dec() {
    uint64_t elapsed = read_timebase() - dec_written_tb;
    if (elapsed >= dec_value)
      return 0;
    return (uint32_t)(dec_value - elapsed);
  }

  bool dec_expired() {
    return dec_irq_pending && (read_timebase() - dec_written_tb) >= dec_value;
  }

  CPUContext() : gpr{0}, fpr{0.0}, ps1{0.0}, cr{}, pc(0), lr(0), ctr(0), xer(0), 
                 msr(0), fpscr(0), srr0(0), srr1(0), gqr{0}, exception_pc(0), is_running(true), vblank_pending(false) {
    // FPSCR[NI] for fp_round_single: the newest context is the running one (the game has one, main.cpp);
    // destruction hands back to the previous one.
    fpscr_live_prev_ = g_fpscr_live;
    g_fpscr_live = &fpscr;
  }
  ~CPUContext() {
    if (g_fpscr_live == &fpscr) g_fpscr_live = fpscr_live_prev_;
  }
  const uint32_t *fpscr_live_prev_ = nullptr;

  void queue_callback(uint32_t cb, uint32_t arg1, uint32_t arg2, bool is_irq = false) {
    std::lock_guard<std::mutex> lock(cb_mutex);
    pending_callbacks.push({cb, arg1, arg2, is_irq});
    pending_cb_count.fetch_add(1, std::memory_order_relaxed);
  }

  void psq_load(uint32_t frD, uint32_t addr, uint32_t W, uint32_t I) {
    uint32_t gqr_val = gqr[I];
    uint32_t ld_type = (gqr_val >> 16) & 0x7;
    // 2^-SCALE for the signed six-bit LD_SCALE field (exact table, Dolphin m_dequantizeTable).
    const float scale = kQuantTables.dequant[(gqr_val >> 24) & 0x3F];

    auto load_element = [&](uint32_t a) -> double {
      switch (ld_type) {
      case 0:
        return mmu.read_f32(a);
      case 4:
        return mmu.read8(a) * scale;
      case 5:
        return mmu.read16(a) * scale;
      case 6:
        return (int8_t)mmu.read8(a) * scale;
      case 7:
        return (int16_t)mmu.read16(a) * scale;
      default:
        return 0.0;
      }
    };

    uint32_t elem_size =
        (ld_type == 0) ? 4 : ((ld_type == 4 || ld_type == 6) ? 1 : 2);

    fpr[frD] = load_element(addr);
    if (W == 0) { 
      ps1[frD] = load_element(addr + elem_size);
    } else { 
      ps1[frD] = 1.0;
    }
  }

  void psq_store(uint32_t frS, uint32_t addr, uint32_t W, uint32_t I) {
    uint32_t gqr_val = gqr[I];
    uint32_t st_type = gqr_val & 0x7;
    // 2^SCALE for the signed six-bit ST_SCALE field (exact table, Dolphin m_quantizeTable).
    const float scale = kQuantTables.quant[(gqr_val >> 8) & 0x3F];

    // Gekko User's Manual 2.3.4.3: SCALE is signed six-bit, integer stores
    // truncate toward zero and saturate. NaN/+Inf select the positive limit.
    // In particular THP uses GQR6=0x3D043D04: -3 divides IDCT output by eight.
    auto quantize = [&](double value, int32_t minimum, int32_t maximum) -> int32_t {
      const float scaled = static_cast<float>(value) * scale;
      if (scaled <= minimum) return minimum;
      if (!(scaled < maximum)) return maximum;
      return static_cast<int32_t>(scaled);
    };

    auto store_element = [&](uint32_t a, double val) {
      switch (st_type) {
      case 0:
        mmu.write_f32(a, (float)val);
        break;
      case 4:
        mmu.write8(a, static_cast<uint8_t>(quantize(val, 0, 255)));
        break;
      case 5:
        mmu.write16(a, static_cast<uint16_t>(quantize(val, 0, 65535)));
        break;
      case 6:
        mmu.write8(a, static_cast<uint8_t>(quantize(val, -128, 127)));
        break;
      case 7:
        mmu.write16(a, static_cast<uint16_t>(quantize(val, -32768, 32767)));
        break;
      }
    };

    uint32_t elem_size =
        (st_type == 0) ? 4 : ((st_type == 4 || st_type == 6) ? 1 : 2);

    store_element(addr, fpr[frS]);
    if (W == 0) {
      store_element(addr + elem_size, ps1[frS]);
    }
  }

  // ---- FP Unavailable (vector 0x800) and the SDK's lazy FPU switching ----
  // Gekko raises FP Unavailable for any FP/paired-single instruction (loads
  // and stores included) while MSR[FP]=0. The Revolution OS depends on it:
  // OSSetCurrentContext clears MSR[FP] (and SRR1[FP] of the context) unless
  // the context owns the FPU (__OSFPUContext, 0x800000D8), and the installed
  // handler OSSwitchFPUContext (0x803A53A0 in SOUE01) then saves the owner's
  // registers (__OSSaveFPUContext), loads the current context's
  // (__OSLoadFPUContext, only if OS_CONTEXT_STATE_FPSAVED), makes the current
  // context the owner and returns with MSR[FP]=1. OSSaveContext/OSLoadContext
  // never touch FPRs, so without this exception every guest thread switch
  // leaks f0-f31 between threads.
  //
  // The guards call this right after an MSR write (mtmsr, rfi) leaves
  // MSR[FP]=0, i.e. the exception is taken eagerly, as if the next
  // instruction were a floating-point one. Register values per context are
  // the same as with the lazy trap; the eager form keeps OS_CURRENT_CONTEXT
  // (0xD4) the FPU owner whenever guest code runs, which is what the HLE
  // interrupt entry/exit (ios.cpp hle_save/load_context) already assume when
  // they store and reload the FPRs of the interrupted context.
  // Only the FPU effects of the exception are applied: the prologue's SRR0/
  // SRR1 and context scratch fields are not written, because there is no
  // faulting instruction yet and the current context may hold the state of an
  // interrupted thread (an interrupt handler running with D4 = that thread).
  static constexpr uint32_t kMsrFp = 0x2000u;
  static constexpr uint32_t kOsCurrentContext = 0x800000D4u;
  static constexpr uint32_t kOsFpuContext = 0x800000D8u;
  static constexpr uint32_t kOsExceptionTable = 0x80003000u; // __OSExceptionTable
  static constexpr uint32_t kFpUnavailableException = 7;    // __OS_EXCEPTION_FLOATING_POINT

  static bool fp_guest_context(uint32_t c) {
    return (c >= 0x80000000u && c < 0x81800000u - 0x2C8u) ||
           (c >= 0x90000000u && c < 0x94000000u - 0x2C8u);
  }

  // True when the installed FP Unavailable handler is the SDK's
  // OSSwitchFPUContext (its first 11 instructions), whose effects we apply.
  bool fp_switch_handler_installed() {
    static uint32_t checked_handler = 0;
    static bool is_sdk_switch = false;
    const uint32_t handler =
        mmu.read32(kOsExceptionTable + kFpUnavailableException * 4);
    if (handler != checked_handler) {
      static constexpr uint32_t kSignature[11] = {
          0x7CA000A6u, 0x60A52000u, 0x7CA00124u, 0x4C00012Cu,
          0x80A4019Cu, 0x60A52000u, 0x7CBB03A6u, 0x3C608000u,
          0x80A300D8u, 0x908300D8u, 0x7C052000u};
      bool match = fp_guest_context(handler);
      for (uint32_t i = 0; match && i < 11; ++i)
        match = mmu.read32(handler + i * 4) == kSignature[i];
      checked_handler = handler;
      is_sdk_switch = match;
    }
    return is_sdk_switch;
  }

  // __OSSaveFPUContext (0x803A4CD0): FPSAVED, f0-f31 doubles at +0x90, the
  // mffs image at +0x190 (FPSCR in the low word), and with HID2[PSE] both
  // lanes as psq_st W=0 I=0 at +0x1C8.
  void fp_save_context(uint32_t c) {
    mmu.write16(c + 0x1A2, mmu.read16(c + 0x1A2) | 1u);
    for (uint32_t i = 0; i < 32; ++i) {
      uint64_t bits;
      std::memcpy(&bits, &fpr[i], 8);
      mmu.write64(c + 0x90 + i * 8, bits);
    }
    mmu.write64(c + 0x190, 0xFFF8000000000000ull | fpscr);
    if (lc_hid2 & 0x20000000u)
      for (uint32_t i = 0; i < 32; ++i)
        psq_store(i, c + 0x1C8 + i * 8, 0, 0);
  }

  // __OSLoadFPUContext (0x803A4BA0): nothing unless FPSAVED; mtfsf 0xFF from
  // +0x190, paired lanes from +0x1C8 (HID2[PSE]), then the PS0 doubles.
  void fp_load_context(uint32_t c) {
    if ((mmu.read16(c + 0x1A2) & 1u) == 0) return;
    fpscr = static_cast<uint32_t>(mmu.read64(c + 0x190));
    if (lc_hid2 & 0x20000000u)
      for (uint32_t i = 0; i < 32; ++i)
        psq_load(i, c + 0x1C8 + i * 8, 0, 0);
    for (uint32_t i = 0; i < 32; ++i) {
      const uint64_t bits = mmu.read64(c + 0x90 + i * 8);
      std::memcpy(&fpr[i], &bits, 8);
    }
  }

  static uint64_t &fp_unavailable_switches() {
    static uint64_t n = 0;
    return n;
  }

  // FP Unavailable taken with MSR[FP]=0: the effects of OSSwitchFPUContext.
  void fp_unavailable() {
    if (g_env_fptrap && g_fptrap_arm_hook) g_fptrap_arm_hook(); // diagnostic only (CPU thread)
    if (!g_env_fp_unavailable_off && fp_switch_handler_installed()) {
      const uint32_t current = mmu.read32(kOsCurrentContext);
      if (fp_guest_context(current)) {
        const uint32_t owner = mmu.read32(kOsFpuContext);
        mmu.write32(kOsFpuContext, current);
        if (owner != current) {
          if (fp_guest_context(owner)) fp_save_context(owner);
          fp_load_context(current);
          ++fp_unavailable_switches();
        }
      }
    }
    msr |= kMsrFp; // rfi from the handler: MSR = SRR1 | MSR[FP]
  }

  // Guard emitted after every MSR write (mtmsr, rfi).
  inline void fp_unavailable_check() {
    if (!(msr & kMsrFp)) fp_unavailable();
  }

  // ---- Paired-single arithmetic (FP track): Dolphin Interpreter_Paired.cpp ----
  // Both lanes are rounded to single (ForceSingle), frC of multiplies is
  // Force25Bit, madds are fused with Gekko's single rounding, ps_nm* keep the
  // sign of a NaN result. Lane-crossing ops read their sources first.
  NWII_FP_INLINE void ps_add(uint32_t D, uint32_t A, uint32_t B) {
    fpr[D] = fp_round_single(fpr[A] + fpr[B]);
    ps1[D] = fp_round_single(ps1[A] + ps1[B]);
  }
  NWII_FP_INLINE void ps_sub(uint32_t D, uint32_t A, uint32_t B) {
    fpr[D] = fp_round_single(fpr[A] - fpr[B]);
    ps1[D] = fp_round_single(ps1[A] - ps1[B]);
  }
  NWII_FP_INLINE void ps_mul(uint32_t D, uint32_t A, uint32_t C) {
    fpr[D] = fp_round_single(fpr[A] * fp_force25(fpr[C]));
    ps1[D] = fp_round_single(ps1[A] * fp_force25(ps1[C]));
  }
  NWII_FP_INLINE void ps_madd(uint32_t D, uint32_t A, uint32_t C, uint32_t B) {
    fpr[D] = fp_round_single(fp_madd_single(fpr[A], fpr[C], fpr[B]));
    ps1[D] = fp_round_single(fp_madd_single(ps1[A], ps1[C], ps1[B]));
  }
  NWII_FP_INLINE void ps_msub(uint32_t D, uint32_t A, uint32_t C, uint32_t B) {
    fpr[D] = fp_round_single(fp_madd_single(fpr[A], fpr[C], -fpr[B]));
    ps1[D] = fp_round_single(fp_madd_single(ps1[A], ps1[C], -ps1[B]));
  }
  NWII_FP_INLINE void ps_nmadd(uint32_t D, uint32_t A, uint32_t C, uint32_t B) {
    fpr[D] = fp_neg_unless_nan(fp_round_single(fp_madd_single(fpr[A], fpr[C], fpr[B])));
    ps1[D] = fp_neg_unless_nan(fp_round_single(fp_madd_single(ps1[A], ps1[C], ps1[B])));
  }
  NWII_FP_INLINE void ps_nmsub(uint32_t D, uint32_t A, uint32_t C, uint32_t B) {
    fpr[D] = fp_neg_unless_nan(fp_round_single(fp_madd_single(fpr[A], fpr[C], -fpr[B])));
    ps1[D] = fp_neg_unless_nan(fp_round_single(fp_madd_single(ps1[A], ps1[C], -ps1[B])));
  }
  NWII_FP_INLINE void ps_div(uint32_t D, uint32_t A, uint32_t B) {
    fpr[D] = fp_round_single(fpr[A] / fpr[B]);
    ps1[D] = fp_round_single(ps1[A] / ps1[B]);
  }
  NWII_FP_INLINE void ps_sum0(uint32_t D, uint32_t A, uint32_t C, uint32_t B) {
    const double s = fp_round_single(fpr[A] + ps1[B]);
    const double t = fp_round_single(ps1[C]);
    fpr[D] = s;
    ps1[D] = t;
  }
  NWII_FP_INLINE void ps_sum1(uint32_t D, uint32_t A, uint32_t C, uint32_t B) {
    const double s = fp_round_single(fpr[C]);
    const double t = fp_round_single(fpr[A] + ps1[B]);
    fpr[D] = s;
    ps1[D] = t;
  }
  NWII_FP_INLINE void ps_muls0(uint32_t D, uint32_t A, uint32_t C) {
    const double c0 = fp_force25(fpr[C]);
    fpr[D] = fp_round_single(fpr[A] * c0);
    ps1[D] = fp_round_single(ps1[A] * c0);
  }
  NWII_FP_INLINE void ps_muls1(uint32_t D, uint32_t A, uint32_t C) {
    const double c1 = fp_force25(ps1[C]);
    fpr[D] = fp_round_single(fpr[A] * c1);
    ps1[D] = fp_round_single(ps1[A] * c1);
  }
  NWII_FP_INLINE void ps_madds0(uint32_t D, uint32_t A, uint32_t C, uint32_t B) {
    const double c0 = fpr[C];
    fpr[D] = fp_round_single(fp_madd_single(fpr[A], c0, fpr[B]));
    ps1[D] = fp_round_single(fp_madd_single(ps1[A], c0, ps1[B]));
  }
  NWII_FP_INLINE void ps_madds1(uint32_t D, uint32_t A, uint32_t C, uint32_t B) {
    const double c1 = ps1[C];
    fpr[D] = fp_round_single(fp_madd_single(fpr[A], c1, fpr[B]));
    ps1[D] = fp_round_single(fp_madd_single(ps1[A], c1, ps1[B]));
  }
  NWII_FP_INLINE void ps_sel(uint32_t D, uint32_t A, uint32_t C, uint32_t B) {
    fpr[D] = (fpr[A] >= -0.0) ? fpr[C] : fpr[B];
    ps1[D] = (ps1[A] >= -0.0) ? ps1[C] : ps1[B];
  }
  // ps_res / ps_rsqrte: Gekko table estimates of frB (Dolphin ApproximateReciprocal[SquareRoot]).
  NWII_FP_INLINE void ps_res(uint32_t D, uint32_t B) {
    const double a = fpr[B], b = ps1[B];
    fpr[D] = fp_fres(a);
    ps1[D] = fp_fres(b);
  }
  NWII_FP_INLINE void ps_rsqrte(uint32_t D, uint32_t B) {
    const double a = fpr[B], b = ps1[B];
    fpr[D] = fp_round_single(fp_frsqrte(a));
    ps1[D] = fp_round_single(fp_frsqrte(b));
  }
  NWII_FP_INLINE void ps_mr(uint32_t D, uint32_t B) {
    fpr[D] = fpr[B];
    ps1[D] = ps1[B];
  }
  NWII_FP_INLINE void ps_neg(uint32_t D, uint32_t B) {
    fpr[D] = -fpr[B];
    ps1[D] = -ps1[B];
  }
  NWII_FP_INLINE void ps_abs(uint32_t D, uint32_t B) {
    fpr[D] = std::abs(fpr[B]);
    ps1[D] = std::abs(ps1[B]);
  }
  NWII_FP_INLINE void ps_nabs(uint32_t D, uint32_t B) {
    fpr[D] = -std::abs(fpr[B]);
    ps1[D] = -std::abs(ps1[B]);
  }

  
  NWII_FP_INLINE void ps_merge00(uint32_t D, uint32_t A, uint32_t B) {
    double a = fpr[A], b = fpr[B];
    fpr[D] = a; ps1[D] = b;
  }
  NWII_FP_INLINE void ps_merge01(uint32_t D, uint32_t A, uint32_t B) {
    double a = fpr[A], b = ps1[B];
    fpr[D] = a; ps1[D] = b;
  }
  NWII_FP_INLINE void ps_merge10(uint32_t D, uint32_t A, uint32_t B) {
    double a = ps1[A], b = fpr[B];
    fpr[D] = a; ps1[D] = b;
  }
  NWII_FP_INLINE void ps_merge11(uint32_t D, uint32_t A, uint32_t B) {
    double a = ps1[A], b = ps1[B];
    fpr[D] = a; ps1[D] = b;
  }
  NWII_FP_INLINE void ps_cmpu0(uint32_t crD, uint32_t A, uint32_t B) {
    const double a = fpr[A], b = fpr[B];
    const bool lt = a < b, gt = a > b, eq = a == b; // all false when unordered
    cr[crD].lt = lt;
    cr[crD].gt = gt;
    cr[crD].eq = eq;
    cr[crD].so = !(lt || gt || eq);
  }
  NWII_FP_INLINE void ps_cmpo0(uint32_t crD, uint32_t A, uint32_t B) {
    const double a = fpr[A], b = fpr[B];
    const bool lt = a < b, gt = a > b, eq = a == b; // all false when unordered
    cr[crD].lt = lt;
    cr[crD].gt = gt;
    cr[crD].eq = eq;
    cr[crD].so = !(lt || gt || eq);
  }
  NWII_FP_INLINE void ps_cmpu1(uint32_t crD, uint32_t A, uint32_t B) {
    const double a = ps1[A], b = ps1[B];
    const bool lt = a < b, gt = a > b, eq = a == b; // all false when unordered
    cr[crD].lt = lt;
    cr[crD].gt = gt;
    cr[crD].eq = eq;
    cr[crD].so = !(lt || gt || eq);
  }
  NWII_FP_INLINE void ps_cmpo1(uint32_t crD, uint32_t A, uint32_t B) {
    const double a = ps1[A], b = ps1[B];
    const bool lt = a < b, gt = a > b, eq = a == b; // all false when unordered
    cr[crD].lt = lt;
    cr[crD].gt = gt;
    cr[crD].eq = eq;
    cr[crD].so = !(lt || gt || eq);
  }
};

void hle_set_ipc_arm_msg(uint32_t req_addr);
void micro_interpret(CPUContext& ctx, uint32_t opcode, uint32_t pc);

bool interpret_one(CPUContext& ctx);
void interpret_step(CPUContext& ctx);
void add_recompiled_range(uint32_t start, uint32_t end);
bool in_recompiled_code(uint32_t pc);

extern bool g_trace_calls;
void trace_call(uint32_t func_addr, CPUContext& ctx);




void note_null_call(uint32_t site, uint32_t lr);

} 
} 
