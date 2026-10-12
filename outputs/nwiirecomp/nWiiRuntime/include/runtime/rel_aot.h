#pragma once
// RELAOT (roadmap M2.7/M2.8): statically recompiled REL modules.
//
// nwiirecomp --rels emits one translation unit per REL module (rels/rel_<id>.cpp)
// plus rels/rel_aot_table.cpp. The generated code is position independent:
//   * every PC-valued constant is (SB + section offset), SB = the section's
//     runtime base recorded when the game's own OSLink has linked the module;
//   * every instruction covered by a relocation reads its (already relocated)
//     immediate / branch displacement from guest RAM at run time, so whatever
//     OSLink / OSUnlink wrote is what executes (same as the interpreter);
//   * calls into the DOL go to the recompiled DOL function directly when the
//     relocated branch resolves to the expected address, otherwise through
//     relaot::call() (REL registry, DOL function table, else the dispatcher).
// Modules are discovered by reading the OS module queue (0x800030C8/CC); guest
// memory is only read, never written. NWII_REL_AOT=0 disables everything.
#include <cstdint>
#include <cstdlib>

namespace nwii {
namespace runtime {
struct CPUContext;
namespace relaot {

using Fn = void (*)(CPUContext &);

struct FuncEntry {     // one recompiled guest function, section-relative
  uint32_t start, end; // [start, end) byte offsets inside the section
  Fn fn;
  // SectionDesc::entries[first_entry .. first_entry + n_entries): the offsets
  // the native function can be entered at (start, return sites, poll resume
  // points, branch / jump-table / pointer targets). Any other pc inside the
  // function is left to the interpreter.
  uint32_t first_entry, n_entries;
};

struct SectionDesc {
  uint32_t index;        // REL section number (executable)
  uint32_t size;         // section size in bytes (must match the linked module)
  uint64_t hash;         // image_hash() of the unrelocated section bytes
  const FuncEntry *funcs; // sorted by start
  uint32_t nfuncs;
  uint32_t *sb;           // generated TU global: runtime base of this section
  const uint8_t **sh;     // generated TU global: host pointer of that base
  const uint32_t *entries; // entry offsets of all functions, ascending
};

struct ModuleDesc {
  uint32_t id;
  const char *name;
  const SectionDesc *secs;
  uint32_t nsecs;
};

struct DolFn {
  uint32_t addr;
  Fn fn;
};

// Interpreter entry hook: if ctx.pc is inside a linked, AOT-registered
// module, run it natively (continuing while the next pc is also AOT code)
// and return true; ctx.pc then holds where execution continues.
bool enter(CPUContext &ctx);

// Call/jump from recompiled REL code to a runtime address `target` with the
// return address `ret` already in ctx.lr. Returns true when the callee ran
// natively and came back to `ret`; false means ctx.pc is set and the caller
// must return to the dispatcher (which resumes at ret later).
bool call(CPUContext &ctx, uint32_t target, uint32_t ret);

// FNV-1a 64 over the section words, with relocatable immediates masked
// (b/bl: low 26 bits; D-form immediate ops: low 16 bits), so the hash of the
// original .rel bytes equals the hash of the linked image in guest RAM.
inline uint64_t image_hash(const uint8_t *p, uint32_t size) {
  uint64_t h = 1469598103934665603ull;
  for (uint32_t i = 0; i + 4 <= size; i += 4) {
    uint32_t w = (uint32_t)p[i] << 24 | (uint32_t)p[i + 1] << 16 |
                 (uint32_t)p[i + 2] << 8 | p[i + 3];
    const uint32_t op = w >> 26;
    if (op == 18)
      w &= 0xFC000003u;
    else if (op == 13 || op == 14 || op == 15 || (op >= 32 && op <= 55))
      w &= 0xFFFF0000u;
    for (int b = 0; b < 4; ++b) {
      h ^= (w >> (24 - 8 * b)) & 0xFF;
      h *= 1099511628211ull;
    }
  }
  return h;
}

// Big-endian reads of the relocated instruction words (host pointer into
// guest MEM1/MEM2; the generated code computes SH + section offset).
inline uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
inline int32_t rd_simm(const uint8_t *word) { // low 16 bits of the word, sign-extended
  return (int32_t)(int16_t)(uint16_t)((uint32_t)word[2] << 8 | word[3]);
}
// Target of a b/bl word at guest address pc (AA honoured like Gekko).
inline uint32_t branch_target(uint32_t w, uint32_t pc) {
  int32_t li = (int32_t)(w & 0x03FFFFFCu);
  if (li & 0x02000000)
    li -= 0x04000000;
  return (w & 2) ? (uint32_t)li : pc + (uint32_t)li;
}

} // namespace relaot
} // namespace runtime
} // namespace nwii
