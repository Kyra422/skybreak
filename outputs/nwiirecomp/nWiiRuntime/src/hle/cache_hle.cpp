// HLE for the SDK cache-maintenance and sync routines (M2.2).
//
// The SDK range routines (OSCache.c, hand-written assembly; SOUE01
// DCInvalidateRange 0x803A4560 ... ICInvalidateRange 0x803A4680) are
//
//     cmplwi r4, 0          ; blelr when the length is 0
//     blelr
//     clrlwi r5, r3, 27     ; r5 = addr & 31
//     add    r4, r4, r5
//     addi   r4, r4, 31
//     srwi   r4, r4, 5      ; number of 32-byte lines
//     mtctr  r4
//   loop:
//     dcbf/dcbst/dcbi/dcbz/icbi 0, r3
//     addi   r3, r3, 32
//     bdnz   loop
//     [sc | sync ; isync]   ; DCFlushRange/DCStoreRange: sc, ICInvalidateRange: sync+isync
//     blr
//
// The runtime keeps no data or instruction cache: guest memory is always
// coherent, the GX parser and the DMA engines read it directly, and the PPC
// interpreter that runs the RELs decodes straight from memory. So dcbf, dcbst,
// dcbi, icbi, sync and isync have no effect here (the generated code and
// interpreter already treat them as no-ops), and only dcbz writes memory.
// Recompiled, each loop iteration still paid a full process_pending_callbacks
// poll at the bdnz back-edge (12-15% of [Sample] PCs in earlier runs).
//
// Each handler reproduces the routine's architectural result exactly: r3, r4,
// r5, ctr and cr0 as the loop leaves them, memory (dcbz), and inst_count -- the
// virtual timebase -- advanced by exactly what the generated code would have
// counted, so the guest-visible clock is unchanged. The only difference is
// that no interrupt is taken in the middle of the loop: pending interrupts are
// delivered at the caller's next poll instead.
//
// inst_count accounting of the generated code: one increment per instruction
// on the straight-line path; a taken branch jumps to a label placed after the
// target's increment, so the loop head (the cache op) counts once and every
// iteration adds 2 (addi, bdnz).

#include "runtime/cache_hle.h"

#include <cstring>
#include <iostream>

using nwii::runtime::CPUContext;

namespace {

struct RangeLoop {
  bool ran = false;          // false: length 0, returned at blelr
  uint64_t iterations = 0;   // bdnz iterations (ctr == 0 at mtctr runs 2^32 times)
  uint32_t first_ea = 0;     // r3 on entry: the first line is first_ea & ~31
};

// cmplwi/blelr, the line-count prologue and the loop's register effects.
RangeLoop range_prologue(CPUContext &ctx) {
  const uint32_t addr = ctx.gpr[3];
  const uint32_t len = ctx.gpr[4];
  ctx.cr[0].lt = false;  // cmplwi r4, 0 (unsigned)
  ctx.cr[0].gt = len != 0;
  ctx.cr[0].eq = len == 0;
  ctx.cr[0].so = ((ctx.xer >> 31) & 1) != 0;
  if (len == 0) {
    ctx.inst_count += 2;  // cmplwi, blelr
    return {};
  }
  const uint32_t r5 = addr & 31;
  const uint32_t lines = ((len + r5 + 31) >> 5) & 0x07FFFFFFu;  // 32-bit wrap as on Gekko
  RangeLoop loop;
  loop.ran = true;
  loop.iterations = lines ? lines : (uint64_t{1} << 32);
  loop.first_ea = addr;
  ctx.gpr[5] = r5;
  ctx.gpr[4] = lines;
  ctx.gpr[3] = addr + static_cast<uint32_t>(loop.iterations * 32);  // mod 2^32
  ctx.ctr = 0;
  // cmplwi, blelr, clrlwi, add, addi, srwi, mtctr, loop head, 2 per iteration
  ctx.inst_count += 8 + 2 * loop.iterations;
  return loop;
}

// dcbz over the loop's lines. Dolphin's dcbz (MMU::ClearDCacheLine) writes the
// zeroed line through WriteToHardware, which drops writes to unmapped physical
// memory with a log message; this does the same instead of the generated
// code's zero_cache_line throw.
void zero_lines(CPUContext &ctx, uint32_t first_ea, uint64_t iterations) {
  constexpr uint64_t kAllLines = uint64_t{1} << 27;  // 32-byte lines in 4 GiB
  const uint64_t distinct = iterations < kAllLines ? iterations : kAllLines;
  uint32_t line = first_ea & ~31u;
  const uint64_t bytes = distinct * 32;
  if (bytes <= 0xFFFFFFFFu && uint64_t{line} + bytes <= (uint64_t{1} << 32)) {
    if (uint8_t *p = ctx.mmu.get_ptr(line, static_cast<uint32_t>(bytes))) {
      std::memset(p, 0, static_cast<size_t>(bytes));
      return;
    }
  }
  uint64_t skipped = 0;
  uint32_t first_skipped = 0;
  for (uint64_t i = 0; i < distinct; ++i, line += 32) {
    if (uint8_t *p = ctx.mmu.get_ptr(line, 32)) {
      std::memset(p, 0, 32);
    } else {
      if (!skipped++)
        first_skipped = line;
    }
  }
  if (skipped) {
    static int reports = 0;
    if (reports++ < 16)
      std::cout << "[HLE DCZeroRange] skipped " << std::dec << skipped
                << " unmapped line(s) from 0x" << std::hex << first_skipped
                << " (range 0x" << first_ea << " lr=0x" << ctx.lr << ")"
                << std::dec << "\n";
  }
}

}  // namespace

extern "C" {

// dcbf loop + sc + blr
void HLE_DCFlushRange(CPUContext &ctx) {
  if (range_prologue(ctx).ran)
    ctx.inst_count += 2;  // sc, blr
}

// dcbst loop + sc + blr
void HLE_DCStoreRange(CPUContext &ctx) {
  if (range_prologue(ctx).ran)
    ctx.inst_count += 2;
}

// dcbf loop + blr
void HLE_DCFlushRangeNoSync(CPUContext &ctx) {
  if (range_prologue(ctx).ran)
    ctx.inst_count += 1;
}

// dcbst loop + blr
void HLE_DCStoreRangeNoSync(CPUContext &ctx) {
  if (range_prologue(ctx).ran)
    ctx.inst_count += 1;
}

// dcbi loop + blr
void HLE_DCInvalidateRange(CPUContext &ctx) {
  if (range_prologue(ctx).ran)
    ctx.inst_count += 1;
}

// dcbz loop + blr: the one routine with a memory effect.
void HLE_DCZeroRange(CPUContext &ctx) {
  const RangeLoop loop = range_prologue(ctx);
  if (!loop.ran)
    return;
  zero_lines(ctx, loop.first_ea, loop.iterations);
  ctx.inst_count += 1;
}

// icbi loop + sync + isync + blr
void HLE_ICInvalidateRange(CPUContext &ctx) {
  if (range_prologue(ctx).ran)
    ctx.inst_count += 3;
}

// PPCSync: sc + blr. The OS system-call vector only toggles HID0[ABE] around a
// sync (see handle_syscall in ios.cpp); no architectural effect here.
void HLE_PPCSync(CPUContext &ctx) { ctx.inst_count += 2; }

}  // extern "C"
