#pragma once
// Gekko/Broadway floating-point semantics shared by the generated DOL/REL code
// (nWiiRecomp emission), the REL interpreter (src/hle/interpreter.cpp) and the
// CPUContext paired-single / quantization helpers (cpu_context.h).
//
// Reference: Dolphin, Source/Core/Core/PowerPC/Interpreter/Interpreter_FPUtils.h
// (ForceSingle, Force25Bit, NI_madd_msub<single>), Interpreter_FloatingPoint.cpp
// (ConvertToInteger, fres/frsqrte, Fill vs SetPS0), Interpreter_Paired.cpp,
// Interpreter_LoadStorePaired.cpp (m_quantizeTable/m_dequantizeTable) and
// Common/FloatUtils.cpp (ApproximateReciprocal[SquareRoot]). FPSCR[NI] is
// honoured by fp_round_single (Dolphin ForceSingle; OSInit 0x803A2A28 calls
// PPCSetFpNonIEEEMode = mtfsb1 29, so SS runs with NI=1); ForceDouble's NI flush
// of double-subnormal results is not modelled, round-to-nearest is assumed
// (FPSCR rounding control is not modelled), FPSCR exception bits are not maintained.
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>
#include <immintrin.h>
#define NWII_FP_X64_MSVC 1
#endif

namespace nwii {
namespace runtime {

// FMA3 + OS AVX state support (CPUID.1:ECX FMA/OSXSAVE/AVX, XCR0 bits 1-2),
// probed once at start-up. Without it fp_fma falls back to std::fma (same
// correctly rounded result, slower). NWII_FP_NOFMA3=1 forces the fallback
// (A/B measurement).
inline const bool g_cpu_has_fma3 = [] {
#if defined(NWII_FP_X64_MSVC)
  if (const char *e = std::getenv("NWII_FP_NOFMA3"); e && e[0] == '1') return false;
  int r[4];
  __cpuid(r, 1);
  const bool fma = (r[2] >> 12) & 1, osxsave = (r[2] >> 27) & 1, avx = (r[2] >> 28) & 1;
  if (!(fma && osxsave && avx)) return false;
  return (_xgetbv(0) & 6) == 6;
#else
  return false;
#endif
}();

// The generated TUs are compiled /O1 /Ob1, where MSVC does not inline these
// helpers on its own; the hot ones are forced inline, rare paths kept out of line.
#if defined(_MSC_VER)
#define NWII_FP_INLINE __forceinline
#define NWII_FP_NOINLINE __declspec(noinline)
#else
#define NWII_FP_INLINE inline __attribute__((always_inline))
#define NWII_FP_NOINLINE __attribute__((noinline))
#endif

// Fused multiply-add a*c+b with a single rounding (IEEE fma).
NWII_FP_INLINE double fp_fma(double a, double c, double b) noexcept {
#if defined(NWII_FP_X64_MSVC)
  if (g_cpu_has_fma3) [[likely]]
    return _mm_cvtsd_f64(_mm_fmadd_sd(_mm_set_sd(a), _mm_set_sd(c), _mm_set_sd(b)));
#endif
  return std::fma(a, c, b);
}

// FPSCR of the guest CPU, read by fp_round_single for FPSCR[NI]. CPUContext
// registers its fpscr here on construction (the game has one live context,
// main.cpp) and clears it on destruction; null = NI 0.
inline const uint32_t *g_fpscr_live = nullptr;
// NWII_FP_IGNORE_NI=1 restores the pre-SWING11 behaviour (NI ignored) for A/B runs.
inline const bool g_fp_ignore_ni = [] {
  const char *e = std::getenv("NWII_FP_IGNORE_NI");
  return e && e[0] == '1';
}();

// fp_round_single, |result| in (0, FLT_MIN]: Dolphin ForceSingle with FPSCR[NI]=1
// flushes a value that is single-subnormal BEFORE rounding to +-0 (also when it
// would round up to FLT_MIN).
NWII_FP_NOINLINE inline double fp_round_single_tiny(double v, float f) noexcept {
  const uint32_t *fpscr = g_fpscr_live;
  if (fpscr && (*fpscr & 0x4u) && !g_fp_ignore_ni &&
      (std::bit_cast<uint64_t>(v) & 0x7FFFFFFFFFFFFFFFull) < 0x3810000000000000ull) // |v| < 2^-126
    return std::bit_cast<double>(std::bit_cast<uint64_t>(v) & 0x8000000000000000ull);
  return static_cast<double>(f);
}

// Dolphin ForceSingle: round to single precision (nearest-even); with FPSCR[NI]=1
// single-subnormal results become +-0 (out-of-line rare path, see above).
NWII_FP_INLINE double fp_round_single(double v) noexcept {
  const float f = static_cast<float>(v);
  if (((std::bit_cast<uint32_t>(f) & 0x7FFFFFFFu) - 1u) < 0x00800000u) [[unlikely]] // 0 < |f| <= FLT_MIN
    return fp_round_single_tiny(v, f);
  return static_cast<double>(f);
}

// -v, except that a NaN keeps its sign (Dolphin fnmadd/fnmsub/ps_nm*).
NWII_FP_INLINE double fp_neg_unless_nan(double v) noexcept { return v != v ? v : -v; }

// Dolphin Force25Bit, subnormal operand: rounding at the operand's own MSB.
NWII_FP_NOINLINE inline double fp_force25_subnormal(uint64_t x) noexcept {
  const uint64_t fraction = x & 0x000FFFFFFFFFFFFFull;
  int64_t keep_mask = static_cast<int64_t>(0xFFFFFFFFF8000000ull);
  uint64_t round = 0x8000000;
  const unsigned shift = static_cast<unsigned>(std::countl_zero(fraction)) - (63 - 52);
  keep_mask >>= shift;
  round >>= shift;
  return std::bit_cast<double>((x & static_cast<uint64_t>(keep_mask)) + (x & round));
}

// Dolphin Force25Bit: the frC operand of single-precision multiplies is rounded
// to a 25-bit mantissa (round half up on bit 27). For non-subnormal x,
// (x & ~0x7FFFFFF) + (x & 0x8000000) == (x + 0x8000000) & ~0xFFFFFFF.
NWII_FP_INLINE double fp_force25(double d) noexcept {
  const uint64_t x = std::bit_cast<uint64_t>(d);
  if (((x & 0x7FFFFFFFFFFFFFFFull) - 1) < 0x000FFFFFFFFFFFFFull) [[unlikely]] // 0 < |d| < 2^-1022
    return fp_force25_subnormal(x);
  return std::bit_cast<double>((x + 0x8000000ull) & 0xFFFFFFFFF0000000ull);
}

// Exact-tie correction of fp_madd_single (Dolphin NI_madd_msub<single>): the
// double result r sits exactly on a single-precision rounding midpoint; move it
// one ulp towards the exact a*c+b so the following rounding is a single rounding.
NWII_FP_NOINLINE inline double fp_madd_single_tie(double a, double c_round, double b, double r) noexcept {
  const uint64_t bits = std::bit_cast<uint64_t>(r);
  const double a_prime = b - r;
  const double b_prime = r + a_prime;
  const double delta_a = fp_fma(a, c_round, a_prime);
  const double delta_b = b - b_prime;
  const double error = delta_a + delta_b;
  if (error != 0.0)
    r = std::bit_cast<double>(((error > 0.0) == (r > 0.0)) ? bits + 1 : bits - 1);
  return r;
}

// Dolphin NI_madd_msub<single=true> before ForceSingle: a*Force25Bit(c)+b
// fused, corrected on an exact double tie so that the following rounding to
// single equals Gekko's single rounding. Pass -b for msub.
NWII_FP_INLINE double fp_madd_single(double a, double c, double b) noexcept {
  const double c_round = fp_force25(c);
  const double r = fp_fma(a, c_round, b);
  if ((std::bit_cast<uint64_t>(r) & 0x000000001FFFFFFFull) == 0x0000000010000000ull) [[unlikely]]
    return fp_madd_single_tie(a, c_round, b, r);
  return r;
}

inline double fp_make_quiet(double v) noexcept {
  return std::bit_cast<double>(std::bit_cast<uint64_t>(v) | 0x0008000000000000ull);
}

// Dolphin Common::ApproximateReciprocal (fres, ps_res): Gekko's table estimate.
inline double fp_fres(double val) noexcept {
  struct BaseAndDec { int32_t base, dec; };
  static constexpr BaseAndDec kFres[32] = {
      {0x7ff800, 0x3e1}, {0x783800, 0x3a7}, {0x70ea00, 0x371}, {0x6a0800, 0x340}, {0x638800, 0x313},
      {0x5d6200, 0x2ea}, {0x579000, 0x2c4}, {0x520800, 0x2a0}, {0x4cc800, 0x27f}, {0x47ca00, 0x261},
      {0x430800, 0x245}, {0x3e8000, 0x22a}, {0x3a2c00, 0x212}, {0x360800, 0x1fb}, {0x321400, 0x1e5},
      {0x2e4a00, 0x1d1}, {0x2aa800, 0x1be}, {0x272c00, 0x1ac}, {0x23d600, 0x19b}, {0x209e00, 0x18b},
      {0x1d8800, 0x17c}, {0x1a9000, 0x16e}, {0x17ae00, 0x15b}, {0x14f800, 0x15b}, {0x124400, 0x143},
      {0x0fbe00, 0x143}, {0x0d3800, 0x12d}, {0x0ade00, 0x12d}, {0x088400, 0x11a}, {0x065000, 0x11a},
      {0x041c00, 0x108}, {0x020c00, 0x106},
  };
  int64_t integral = std::bit_cast<int64_t>(val);
  const int64_t mantissa = integral & ((1LL << 52) - 1);
  const int64_t sign = integral & static_cast<int64_t>(1ULL << 63);
  int64_t exponent = integral & (0x7FFLL << 52);
  if (mantissa == 0 && exponent == 0) return std::copysign(std::numeric_limits<double>::infinity(), val);
  if (exponent == (0x7FFLL << 52)) {
    if (mantissa == 0) return std::copysign(0.0, val);
    return fp_make_quiet(val);
  }
  if (exponent < (895LL << 52)) return std::copysign(static_cast<double>(std::numeric_limits<float>::max()), val);
  if (exponent >= (1149LL << 52)) return std::copysign(0.0, val);
  exponent = (0x7FDLL << 52) - exponent;
  const int i = static_cast<int>(mantissa >> 37);
  const BaseAndDec &entry = kFres[i / 1024];
  integral = sign | exponent;
  integral |= static_cast<int64_t>(entry.base - (entry.dec * (i % 1024) + 1) / 2) << 29;
  return std::bit_cast<double>(integral);
}

// Dolphin Common::ApproximateReciprocalSquareRoot (frsqrte, ps_rsqrte).
inline double fp_frsqrte(double val) noexcept {
  struct BaseAndDec { int32_t base, dec; };
  static constexpr BaseAndDec kRsqrte[32] = {
      {0x1a7e800, -0x568}, {0x17cb800, -0x4f3}, {0x1552800, -0x48d}, {0x130c000, -0x435},
      {0x10f2000, -0x3e7}, {0x0eff000, -0x3a2}, {0x0d2e000, -0x365}, {0x0b7c000, -0x32e},
      {0x09e5000, -0x2fc}, {0x0867000, -0x2d0}, {0x06ff000, -0x2a8}, {0x05ab800, -0x283},
      {0x046a000, -0x261}, {0x0339800, -0x243}, {0x0218800, -0x226}, {0x0105800, -0x20b},
      {0x3ffa000, -0x7a4}, {0x3c29000, -0x700}, {0x38aa000, -0x670}, {0x3572000, -0x5f2},
      {0x3279000, -0x584}, {0x2fb7000, -0x524}, {0x2d26000, -0x4cc}, {0x2ac0000, -0x47e},
      {0x2881000, -0x43a}, {0x2665000, -0x3fa}, {0x2468000, -0x3c2}, {0x2287000, -0x38e},
      {0x20c1000, -0x35e}, {0x1f12000, -0x332}, {0x1d79000, -0x30a}, {0x1bf4000, -0x2e6},
  };
  int64_t integral = std::bit_cast<int64_t>(val);
  int64_t mantissa = integral & ((1LL << 52) - 1);
  const int64_t sign = integral & static_cast<int64_t>(1ULL << 63);
  int64_t exponent = integral & (0x7FFLL << 52);
  if (mantissa == 0 && exponent == 0)
    return sign ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
  if (exponent == (0x7FFLL << 52)) {
    if (mantissa == 0) return sign ? std::numeric_limits<double>::quiet_NaN() : 0.0;
    return fp_make_quiet(val);
  }
  if (sign) return std::numeric_limits<double>::quiet_NaN();
  if (!exponent) {
    do {
      exponent -= 1LL << 52;
      mantissa <<= 1;
    } while (!(mantissa & (1LL << 52)));
    mantissa &= (1LL << 52) - 1;
    exponent += 1LL << 52;
  }
  const int64_t exponent_lsb = exponent & (1LL << 52);
  exponent = ((0x3FFLL << 52) - ((exponent - (0x3FELL << 52)) / 2)) & (0x7FFLL << 52);
  integral = sign | exponent;
  const int i = static_cast<int>((exponent_lsb | mantissa) >> 37);
  const BaseAndDec &entry = kRsqrte[i / 2048];
  integral |= static_cast<int64_t>(entry.base + entry.dec * (i % 2048)) << 26;
  return std::bit_cast<double>(integral);
}

// fctiw (toward_zero=false, FPSCR[RN]=nearest) / fctiwz result register bits:
// Dolphin ConvertToInteger with FPSCR[VE]=0 (0xFFF80000 high word, saturation,
// NaN -> 0x80000000, bit 32 set for a zero result of a negative operand).
NWII_FP_NOINLINE inline uint64_t fp_fctiw_bits_slow(double b, bool toward_zero) noexcept {
  uint32_t value;
  if (b != b) {
    value = 0x80000000u;
  } else {
    const double rounded = toward_zero ? std::trunc(b) : std::nearbyint(b);
    if (rounded >= 2147483648.0) value = 0x7FFFFFFFu;
    else if (rounded < -2147483648.0) value = 0x80000000u;
    else value = static_cast<uint32_t>(static_cast<int32_t>(rounded));
  }
  uint64_t result = 0xFFF8000000000000ull | value;
  if (value == 0 && std::signbit(b)) result |= 0x100000000ull;
  return result;
}
NWII_FP_INLINE uint64_t fp_fctiw_bits(double b, bool toward_zero) noexcept {
  if (std::fabs(b) < 2147483647.0 && b != 0.0) [[likely]] { // in range, no NaN, no signed zero
#if defined(NWII_FP_X64_MSVC)
    const uint32_t value = static_cast<uint32_t>(toward_zero ? _mm_cvttsd_si32(_mm_set_sd(b)) : _mm_cvtsd_si32(_mm_set_sd(b)));
#else
    const uint32_t value = static_cast<uint32_t>(static_cast<int32_t>(toward_zero ? std::trunc(b) : std::nearbyint(b)));
#endif
    uint64_t result = 0xFFF8000000000000ull | value;
    if (value == 0 && b < 0.0) result |= 0x100000000ull; // -0.x -> 0 with the sign bit
    return result;
  }
  return fp_fctiw_bits_slow(b, toward_zero);
}

// psq_l / psq_st scale factors for the signed six-bit GQR SCALE field (index =
// raw field): dequantize 2^-s, quantize 2^s (Dolphin m_dequantizeTable /
// m_quantizeTable). All entries are exact powers of two.
struct QuantTables {
  float dequant[64];
  float quant[64];
};
inline constexpr QuantTables kQuantTables = [] {
  QuantTables t{};
  for (int i = 0; i < 64; ++i) {
    const int s = i < 32 ? i : i - 64; // signed six-bit scale
    float up = 1.0f, down = 1.0f;
    for (int k = 0; k < (s < 0 ? -s : s); ++k) { up *= 2.0f; down *= 0.5f; }
    t.quant[i] = s >= 0 ? up : down;   // 2^s
    t.dequant[i] = s >= 0 ? down : up; // 2^-s
  }
  return t;
}();

} // namespace runtime
} // namespace nwii
