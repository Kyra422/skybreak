#include "recompiler/recompiler.h"
#include "ppc/instruction.h"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <unordered_set>
#include <algorithm>
#include <cstring>
#include <map>
#include <vector>

namespace nwii {
namespace recomp {

// RELAOT: PC literals and relocatable immediates. In DOL mode each prints the
// exact character sequence (and leaves the same stream flags) as the inline
// code it replaced, so the DOL export is byte-identical.
std::ostream &operator<<(std::ostream &os, const Recompiler::PcLit &p) {
  if (p.rel) {
    os << "(SB + 0x" << std::hex << std::uppercase << (p.v - p.rel->va_base)
       << std::dec << "u)";
    return os;
  }
  os << "0x" << std::hex;
  if (p.upper)
    os << std::uppercase;
  os << p.v << std::dec;
  return os;
}
std::ostream &operator<<(std::ostream &os, const Recompiler::ImmS &s) {
  if (s.rel)
    return os << "RIMM";
  return os << s.v;
}
std::ostream &operator<<(std::ostream &os, const Recompiler::ImmHi &s) {
  if (s.rel)
    return os << "(int32_t)((uint32_t)RIMM << 16)";
  return os << (s.v << 16);
}
std::ostream &operator<<(std::ostream &os, const Recompiler::ImmX &s) {
  if (s.rel)
    return os << "(uint64_t)(uint32_t)RIMM";
  return os << "0x" << std::hex << std::uppercase << s.v << std::dec << "ull";
}

// Record forms ('.') and the integer compares write a whole CR field: LT/GT/EQ
// from the signed 32-bit result and SO copied from XER[SO] (Dolphin
// Interpreter Helper_UpdateCR0 / Helper_IntCompare). Every CR0 update the
// generator emits goes through here so no form can leave a stale bit behind.
static void emit_cr0(std::ostream &out, uint32_t reg,
                     const char *indent = "    ") {
  out << indent << "{ const int32_t cr0v = (int32_t)ctx.gpr[" << reg
      << "]; ctx.cr[0].lt = cr0v < 0; ctx.cr[0].gt = cr0v > 0; "
         "ctx.cr[0].eq = cr0v == 0; ctx.cr[0].so = (ctx.xer >> 31) & 1; }\n";
}

// XER[CA] from the 33rd bit of a 64-bit sum held in `res`.
static void emit_set_ca_from_res(std::ostream &out,
                                 const char *indent = "        ") {
  out << indent
      << "ctx.xer = (ctx.xer & ~0x20000000u) | ((uint32_t)(res >> 32) & 1u) << 29;"
         " // CA\n";
}

// Effective-address base of an X/D-form access: (rA|0).
static std::string ea_base(uint32_t rA) {
  return rA == 0 ? std::string("0u") : "ctx.gpr[" + std::to_string(rA) + "]";
}

Recompiler::Recompiler(const analyzer::Analyzer &analyzer,
                       const SymbolTable *symbols,
                       const RecompilerConfig &config)
    : analyzer_(analyzer), symbols_(symbols), config_(config) {}

static const std::unordered_set<std::string> &hle_handlers() {
  // Native handlers that actually exist in nWiiRuntime. Binding is by symbol
  // name, so any game whose symbols the analyzer can name gets the whole seam
  // with no per-game configuration; [hle_hooks] in the TOML stays available for
  // overrides and for games where a symbol is missed.
  //
  // Deliberately conservative: only handlers that have been run and checked in
  // a real game are listed. Binding DVDReadAsyncPrio automatically loaded the
  // game's DLL as zeros and gave a black screen, so disc, audio, IOS and the
  // remaining VI entry points stay out until each is verified on its own. They
  // can still be forced per game through [hle_hooks].
  //
  // GXLoadTexMtxImm and GXCallDisplayList are deliberately absent: both are
  // implemented but broke rendering in Mario Party 7, so they stay opt-in via
  // the TOML until that is understood.
  //
  // Only STATELESS GX entry points may be name-bound. The SDK GX state setters
  // (GXSetZMode, GXSetBlendMode, GXSetCullMode, GXSetCurrentMtx, GXSetDstAlpha,
  // GXSetNumTevStages, GXSetScissor, GXSetZCompLoc, GXSetAlphaCompare) keep a
  // shadow in __GXData (zmode 0x228, cmode0 0x220, cmode1 0x224, peCtrl 0x22C,
  // genMode 0x254 + dirtyState 0x5FC, suScis 0x148/0x14C, matIdxA 0x80) that
  // later recompiled SDK code writes back to the hardware (GXCopyDisp/GXCopyTex
  // restore zmode/cmode0, GXSetColorUpdate writes cmode0, __GXSetDirtyState
  // writes genMode and matIdxA, GXGetScissor reads suScis). A native seam that
  // skips the shadow makes those later writes stale -- e.g. BP 0x40 = 0x0F at
  // every XFB copy in SOUE01, so Z was never cleared (GXSEAM.md). They are
  // therefore recompiled from the game's own SDK code. GXLoadPosMtxImm and
  // GXLoadNrmMtxImm touch no __GXData state and emit the same FIFO bytes.
  static const std::unordered_set<std::string> s = {
      "GXLoadNrmMtxImm",
      "GXLoadPosMtxImm",
      "OSAllocFromHeap",
      "OSCreateHeap",
      "OSFreeToHeap",
      "OSInitAlloc",
      "OSReport",
      "PADInit",
      "PADRead",
      "VIWaitForRetrace",
  };
  return s;
}

// CodeWarrior C++ mangling appends "__" and then a qualifier or a signature to
// the plain name: "__Q<n>..." (nested class/namespace), "__<len><class>..."
// (class), "__F..." (function signature), "__C..." (const). So
// GXSetBlendMode__Q34nw4r3g3d9ResMatPixF12_GXBlendMode... is the method
// nw4r::g3d::ResMatPix::GXSetBlendMode, not the SDK C function GXSetBlendMode.
// A plain name may itself end in '_' (EGG::StateGX::GXSetScissor_ mangles to
// GXSetScissor___Q23EGG7StateGXF...), so look past the whole underscore run.
// A signature-database library tag ("..._gx_a_GXGeometry_o") starts lowercase
// and is not a mangling.
static bool is_cxx_mangled(const std::string &name) {
  for (size_t at = name.find("__", 1); at != std::string::npos;
       at = name.find("__", at)) {
    size_t end = at;
    while (end < name.size() && name[end] == '_')
      ++end;
    if (end == name.size())
      break;
    const char c = name[end];
    if ((c >= '0' && c <= '9') || c == 'Q' || c == 'F' || c == 'C')
      return true;
    at = end;
  }
  return false;
}

// Symbols out of the signature database can carry the library they came from,
// e.g. "GXSetCullMode\tgx.a GXGeometry.o" -- match on the bare name.
// The result is the name a native seam is matched on; a C++ member or
// overload has no seam name at all (empty result), because the seams
// implement only the SDK's C functions and read their C arguments from r3...
// whereas a method receives `this` in r3.
static std::string bare_symbol(const std::string &prefixed_name,
                               const std::string &prefix = "") {
  const std::string name = !prefix.empty() && prefixed_name.starts_with(prefix)
                               ? prefixed_name.substr(prefix.size())
                               : prefixed_name;
  const size_t space = name.find_first_of(" \t");
  if (is_cxx_mangled(space == std::string::npos ? name : name.substr(0, space)))
    return std::string();
  size_t cut = space;
  if (cut == std::string::npos) {
    // By this point the name has usually been sanitised into an identifier,
    // so the separator that followed the function name is now "__".
    cut = name.find("__", 1);
  }
  std::string out = cut == std::string::npos ? name : name.substr(0, cut);
  // Symbols also arrive carrying their own address, e.g. GXSetZMode_800C0CC0.
  if (out.size() > 9) {
    const size_t us = out.size() - 9;
    if (out[us] == '_' &&
        out.find_first_not_of("0123456789ABCDEFabcdef", us + 1) ==
            std::string::npos)
      out.resize(us);
  }
  return out;
}

static bool is_hle_function(const std::string &name) {
  return hle_handlers().count(bare_symbol(name)) != 0;
}

static std::string resolve_hle_target(uint32_t entry_address,
                                      const analyzer::Function &func,
                                      const std::string &base_name,
                                      const SymbolTable *symbols,
                                      const RecompilerConfig &config) {
  // Explicit per-game hooks override both signature matches and SDK names.
  const auto hook = config.hle_hooks.find(entry_address);
  if (hook != config.hle_hooks.end())
    return hook->second;
  // Short instruction signatures can match unrelated game functions with
  // the same prologue. A supplied CSV symbol takes precedence over guesses.
  if (symbols && symbols->has_authoritative_symbol(entry_address))
    return is_hle_function(base_name) ? base_name : std::string();
  if (!func.hle_hook_name.empty())
    return func.hle_hook_name;
  return is_hle_function(base_name) ? base_name : std::string();
}

static bool has_hle_target(uint32_t address, const analyzer::Analyzer &analyzer,
                           const SymbolTable *symbols,
                           const RecompilerConfig &config) {
  const std::string base_name = symbols && symbols->has_symbol(address)
      ? bare_symbol(symbols->get_symbol(address), config.symbol_prefix)
      : std::string();
  const auto func = analyzer.get_functions().find(address);
  if (func != analyzer.get_functions().end())
    return !resolve_hle_target(address, func->second, base_name, symbols, config).empty();
  return config.hle_hooks.count(address) != 0 || is_hle_function(base_name);
}

void Recompiler::build_emitted_names() {
  emitted_names_.clear();
  base_names_.clear();
  std::set<std::string> taken;
  for (const auto &[start_addr, func] : analyzer_.get_functions()) {
    std::string func_name;
    if (symbols_ && symbols_->has_symbol(start_addr)) {
      func_name = symbols_->get_symbol(start_addr);
    } else {
      std::stringstream ss;
      ss << "func_" << std::hex << std::uppercase << std::setfill('0')
         << std::setw(8) << start_addr;
      func_name = ss.str();
    }
    // The symbol name before any uniquifying suffix. The seam matches on
    // this; the wrapper itself must never carry it, or it would collide with
    // the native handler's own symbol at link time.
    base_names_.push_back(bare_symbol(func_name, config_.symbol_prefix));
    if (taken.count(func_name)) {
      std::stringstream ss;
      ss << func_name << "_" << std::hex << std::uppercase << start_addr;
      func_name = ss.str();
    }
    taken.insert(func_name);
    emitted_names_[start_addr] = func_name;
  }
}

std::string Recompiler::direct_target_name(uint32_t target) const {
  if (rel_) { // RELAOT: functions of the REL module being emitted
    const auto it = rel_->funcs.find(target - rel_->va_base);
    return it == rel_->funcs.end() ? std::string() : it->second;
  }
  if (!emitted_names_.empty()) {
    // Sanitised symbols collide (29 times in SOUE01: static functions,
    // __sinit_*, state-machine thunks); only the address identifies the
    // function, so never resolve a call through the symbol name.
    const auto it = emitted_names_.find(target);
    return it == emitted_names_.end() ? std::string() : it->second;
  }
  // generate_function_cpp() runs without an export-wide name table.
  if (symbols_ && symbols_->has_symbol(target)) {
    std::string name = symbols_->get_symbol(target);
    if (name.find("loc_") == 0)
      name.replace(0, 4, "func_");
    return name;
  }
  if (analyzer_.get_functions().find(target) == analyzer_.get_functions().end())
    return std::string();
  std::stringstream ss;
  ss << "func_" << std::hex << std::uppercase << std::setfill('0')
     << std::setw(8) << target;
  return ss.str();
}


std::vector<std::string> Recompiler::generate_cpp(uint32_t entry_point) {
  std::vector<std::string> generated_files;

  std::cout << "[Recompiler] Starting C++ generation. Total functions to emit: "
            << analyzer_.get_functions().size() << "\n";

  auto emit_custom_hooks = [this](std::ostream &out) {
    std::set<std::string> names;
    for (const auto &[address, name] : config_.hle_hooks) {
      if (names.insert(name).second)
        out << "extern \"C\" void " << name
            << "(nwii::runtime::CPUContext& ctx);\n";
    }
  };

  auto emit_headers = [&emit_custom_hooks](std::ostream &out) {
    out << "#include \"runtime/cpu_context.h\"\n";
    out << "#include \"runtime/config.h\"\n";
    out << "#include <stdint.h>\n";
    out << "#include <bit>\n";
    out << "#include <thread>\n";
    out << "#include <chrono>\n";

    out << "#include <cmath>\n";
    out << "#include <iostream>\n";
    out << "#include <cstdlib>\n\n";
    out << "using namespace nwii::runtime;\n\n";
    emit_custom_hooks(out);
  };

  if (config_.split_output) {
    
    std::string header_path = config_.output_dir + "/functions.h";
    std::ofstream hout(header_path);
    hout << "#pragma once\n";
    hout << "#include \"runtime/cpu_context.h\"\n\n";
    emit_custom_hooks(hout);
    hout << "extern \"C\" {\n";
    hout << "void OSReport(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_Open(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_OpenAsync(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_Close(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_Read(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_Ioctl(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_IoctlAsync(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_Ioctlv(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_IoctlvAsync(nwii::runtime::CPUContext& ctx);\n";
    hout << "void iosAlloc(nwii::runtime::CPUContext& ctx);\n";
    hout << "void iosFree(nwii::runtime::CPUContext& ctx);\n";
    hout << "void VIInit(nwii::runtime::CPUContext& ctx);\n";
    hout << "void VIConfigure(nwii::runtime::CPUContext& ctx);\n";
    hout << "void VIConfigurePan(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXInit(nwii::runtime::CPUContext& ctx);\n";
    hout << "void PADInit(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSInit(nwii::runtime::CPUContext& ctx);\n";
    hout << "void VISetBlack(nwii::runtime::CPUContext& ctx);\n";
    hout << "void VIGetNextField(nwii::runtime::CPUContext& ctx);\n";
    hout << "void VIWaitForRetrace(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSDisableInterrupts(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSEnableInterrupts(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSRestoreInterrupts(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSGetTime(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSTicksToMilliseconds(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSGetArenaLo(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSGetArenaHi(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSSetArenaLo(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSSetArenaHi(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXInitFifoBase(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXInitFifoPtrs(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetCPUFifo(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXGetCPUFifo(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetCopyClear(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXBegin(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXEnd(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetVtxDesc(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetVtxAttrFmt(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXLoadPosMtxImm(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetCullMode(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetNumTevStages(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetBlendMode(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetZMode(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetZCompLoc(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetAlphaCompare(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetDstAlpha(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetScissor(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXCallDisplayList(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetCullMode(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetNumTevStages(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetBlendMode(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetZMode(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetZCompLoc(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetAlphaCompare(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetDstAlpha(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetScissor(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXLoadNrmMtxImm(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXLoadTexMtxImm(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetCurrentMtx(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_Write(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_Seek(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_CloseAsync(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_ReadAsync(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_WriteAsync(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_SeekAsync(nwii::runtime::CPUContext& ctx);\n";
    hout << "void AXInit(nwii::runtime::CPUContext& ctx);\n";
    hout << "void AXAcquireVoice(nwii::runtime::CPUContext& ctx);\n";
    hout << "void AXFreeVoice(nwii::runtime::CPUContext& ctx);\n";
    hout << "void AXSetVoiceState(nwii::runtime::CPUContext& ctx);\n";
    hout << "void AXSetVoiceMix(nwii::runtime::CPUContext& ctx);\n";
    hout << "void AXSetVoiceAdpcm(nwii::runtime::CPUContext& ctx);\n";
    hout << "void AXSetVoiceSrc(nwii::runtime::CPUContext& ctx);\n";
    hout << "void AXSetVoiceOffsets(nwii::runtime::CPUContext& ctx);\n";
    hout << "void DVDInit(nwii::runtime::CPUContext& ctx);\n";
    hout << "void DVDOpen(nwii::runtime::CPUContext& ctx);\n";
    hout << "void DVD_Callback(nwii::runtime::CPUContext& ctx);\n";
    hout << "void DVDReadAsyncPrio(nwii::runtime::CPUContext& ctx);\n";
    hout << "void DVDClose(nwii::runtime::CPUContext& ctx);\n";
    hout << "void DVDGetDriveStatus(nwii::runtime::CPUContext& ctx);\n";
    hout << "void DVDReadPrio(nwii::runtime::CPUContext& ctx);\n";
    hout << "void PADRead(nwii::runtime::CPUContext& ctx);\n";
    hout << "void WPADInit(nwii::runtime::CPUContext& ctx);\n";
    hout << "void WPADRead(nwii::runtime::CPUContext& ctx);\n";
    hout << "void KPADInit(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSInitAlloc(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSCreateHeap(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSAllocFromHeap(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSFreeToHeap(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSGetCurrentThread(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSSleepThread(nwii::runtime::CPUContext& ctx);\n";
    hout << "}\n";

    build_emitted_names();
    std::vector<std::string> all_func_names;
    const std::vector<std::string> &all_base_names = base_names_;

    hout << "extern \"C\" {\n";
    for (const auto &[start_addr, func] : analyzer_.get_functions()) {
      const std::string &func_name = emitted_names_.at(start_addr);
      all_func_names.push_back(func_name);

      hout << "void " << func_name << "(nwii::runtime::CPUContext& ctx);\n";
    }
    hout << "}\n";
    hout.close();

    int file_idx = 0;
    int func_idx = 0;
    int count_in_current_file = 0;
    std::ofstream out;

    for (const auto &[start_addr, func] : analyzer_.get_functions()) {
      if (count_in_current_file == 0) {
        if (out.is_open()) {
          out.close();
          std::cout << "[Recompiler] Generated " << generated_files.back()
                    << "\n";
        }
        std::stringstream ss;
        ss << "output_" << file_idx++ << ".cpp";
        std::string fname = ss.str();
        generated_files.push_back(fname);
        out.open(config_.output_dir + "/" + fname);
        emit_headers(out);
        out << "#include \"functions.h\"\n\n";
      }

      std::string func_name = all_func_names[func_idx];
      const std::string &base_name = all_base_names[func_idx++];
      const std::string target = resolve_hle_target(start_addr, func, base_name, symbols_, config_);
      if (!target.empty()) {
        if (target != func_name) {
          out << "void " << func_name << "(nwii::runtime::CPUContext& ctx) {\n";
          out << "    " << target << "(ctx);\n";
          out << "    ctx.pc = ctx.lr;\n";
          out << "}\n\n";
        }
      } else {
        emit_function(out, func, func_name);
      }

      count_in_current_file += func.instructions.size();
      if (count_in_current_file >= config_.instructions_per_file) {
        count_in_current_file = 0;
      }
    }
    if (out.is_open()) {
      out.close();
      std::cout << "[Recompiler] Generated " << generated_files.back() << "\n";
    }

    std::string main_name = "main_output.cpp";
    generated_files.push_back(main_name);
    out.open(config_.output_dir + "/" + main_name);
    emit_headers(out);
    out << "#if defined(__GNUC__) || defined(__clang__)\n";
    out << "#pragma GCC optimize (\"O0\")\n";
    out << "#endif\n";
    out << "#if defined(__clang__)\n";
    out << "#pragma clang optimize off\n";
    out << "#endif\n\n";
    out << "#include \"functions.h\"\n\n";
    // M1.11: this export carries the MSR[FP] guards after mtmsr/rfi; the
    // REL interpreter applies the same guards only when this is set.
    out << "static const bool nwii_fp_unavailable_guards =\n"
           "    (nwii::runtime::g_fp_unavailable_emulation = true);\n\n";

    std::string entry_func = direct_target_name(entry_point);
    if (!entry_func.empty()) {
      // emitted (possibly uniquified) name of the function at the entry point
    } else if (symbols_ && symbols_->has_symbol(entry_point)) {
      entry_func = symbols_->get_symbol(entry_point);
    } else {
      std::stringstream ss;
      ss << "func_" << std::hex << std::uppercase << std::setfill('0')
         << std::setw(8) << entry_point;
      entry_func = ss.str();
    }

    out << "// --- Function Bounds Table ---\n";
    out << "struct FuncBound { uint32_t start; uint32_t end; "
           "void(*func)(CPUContext&); };\n";
    out << "static const FuncBound g_func_bounds[] = {\n";

    func_idx = 0;
    // RELAOT: hull of the table, so the dispatcher can skip the linear scan
    // for addresses no entry can contain (REL modules, heap code).
    uint32_t bounds_lo = 0xFFFFFFFFu, bounds_hi = 0;
    // PERF3 / M2.3: the table rows in scan order, for the dispatch segments.
    struct BoundRow { uint32_t start, end; std::string name; };
    std::vector<BoundRow> bound_rows;
    for (const auto &[start_addr, func] : analyzer_.get_functions()) {
      std::string func_name = all_func_names[func_idx];
      bool is_hle = !resolve_hle_target(start_addr, func, all_base_names[func_idx], symbols_, config_).empty();
      ++func_idx;
      if (!is_hle) {
        out << "    {0x" << std::hex << std::uppercase << start_addr << ", 0x"
            << func.end_address << ", " << func_name << "},\n";
        bounds_lo = std::min(bounds_lo, start_addr);
        bounds_hi = std::max(bounds_hi, func.end_address);
        bound_rows.push_back({start_addr, func.end_address, func_name});
      }
    }
    out << "    {0, 0, nullptr}\n";
    out << "};\n\n";

    // PERF3 / roadmap M2.3: the dispatcher's fallback scanned g_func_bounds
    // linearly (29.5k rows) and took the FIRST row whose [start, end)
    // contains the target. Rows overlap (analyzer extents), so the scan's
    // answer is precomputed here as disjoint segments: each row claims the
    // parts of its range that no earlier row claimed. A binary search over
    // the segments returns exactly the row the scan returned.
    // Checked by outputs/pc-tests/perf3/check_dispatch_segments.py.
    std::vector<BoundRow> segs;
    {
      std::map<uint32_t, uint32_t> claimed;  // disjoint [start, end), merged
      for (const auto &row : bound_rows) {
        if (row.end <= row.start) continue;  // empty row: the scan never matches it
        // gaps of [row.start, row.end) not covered by `claimed`
        uint32_t pos = row.start;
        auto it = claimed.upper_bound(pos);
        if (it != claimed.begin()) {
          auto prev = std::prev(it);
          if (prev->second > pos) pos = prev->second;
        }
        while (pos < row.end) {
          if (it != claimed.end() && it->first <= pos) {
            pos = std::max(pos, it->second);
            ++it;
            continue;
          }
          const uint32_t gap_end = it == claimed.end() ? row.end : std::min(row.end, it->first);
          segs.push_back({pos, gap_end, row.name});
          pos = gap_end;
        }
        // merge the row into `claimed`
        uint32_t s = row.start, e = row.end;
        auto m = claimed.upper_bound(s);
        if (m != claimed.begin()) {
          auto pm = std::prev(m);
          if (pm->second >= s) {
            s = pm->first;
            e = std::max(e, pm->second);
            claimed.erase(pm);
          }
        }
        m = claimed.lower_bound(s);
        while (m != claimed.end() && m->first <= e) {
          e = std::max(e, m->second);
          m = claimed.erase(m);
        }
        claimed[s] = e;
      }
      std::sort(segs.begin(), segs.end(),
                [](const BoundRow &a, const BoundRow &b) { return a.start < b.start; });
    }
    out << "// --- Dispatch segments (PERF3 / M2.3) ---\n";
    out << "// First-match owner of every address g_func_bounds covers, as disjoint\n";
    out << "// segments sorted by start: a binary search gives the old scan's row.\n";
    out << "struct DispatchSeg { uint32_t start; uint32_t end; "
           "void(*func)(CPUContext&); };\n";
    out << "static const DispatchSeg g_dispatch_segs[] = {\n";
    for (const auto &sg : segs)
      out << "    {0x" << std::hex << std::uppercase << sg.start << ", 0x" << sg.end << std::dec
          << ", " << sg.name << "},\n";
    out << "};\n";
    out << "static constexpr size_t g_dispatch_seg_count = " << std::dec << segs.size() << ";\n\n";

    out << "// --- Entry Point Wrapper ---\n";
    out << "extern \"C\" void run_game(nwii::runtime::CPUContext& ctx) {\n";
    out << "    static uint32_t pc_history[10] = {0};\n";
    out << "    static int pc_history_idx = 0;\n";
    out << "    if (ctx.pc == 0) ctx.pc = 0x" << std::hex << std::uppercase
        << entry_point << std::dec << ";\n";
    out << "    while (ctx.is_running) {\n"
           "      pc_history[pc_history_idx] = ctx.pc;\n"
           "      pc_history_idx = (pc_history_idx + 1) % 10;\n"
           "      if (ctx.pc == 0) {\n"
           "          static bool warned_zero = false;\n"
           "          if (!warned_zero) {\n"
           "              warned_zero = true;\n"
           "              std::cout << \"[run_game] PC=0 (branch to NULL), last PCs:\";\n"
           "              for (int i = 0; i < 10; ++i)\n"
           "                  std::cout << \" 0x\" << std::hex << pc_history[(pc_history_idx + 9 - i) % 10];\n"
           "              std::cout << \" lr=0x\" << ctx.lr << std::dec << std::endl;\n"
           "          }\n"
           "          if ((ctx.msr & 0x8000) == 0) ctx.msr |= 0x8000; // Force "
           "EE=1 in idle\n"
           "          process_pending_callbacks(ctx);\n"
           "          if (ctx.pc == 0) {\n"
           "              "
           "std::this_thread::sleep_for(std::chrono::milliseconds(1));\n"
           "          }\n"
           "          continue;\n"
           "      }\n";
    out << "      if (_setjmp(ctx.exception_jmp_buf) == 0) {\n";
    out << "        process_pending_callbacks(ctx);\n";
    out << "        if (ctx.pc == 0xFFFFFFFC) {\n";
    out << "            if (!ctx.backup_stack.empty()) {\n";
    out << "                auto& bk = ctx.backup_stack.top();\n";
    out << "                ctx.gpr = bk.gpr; ctx.fpr = bk.fpr; ctx.ps1 = "
           "bk.ps1;\n";
    out << "                ctx.cr = bk.cr; ctx.lr = bk.lr; ctx.ctr = "
           "bk.ctr;\n";
    out << "                ctx.xer = bk.xer; ctx.pc = bk.pc;\n"
           "                ctx.srr0 = bk.srr0; ctx.srr1 = bk.srr1;\n"
           "                ctx.msr = bk.msr; ctx.fpscr = bk.fpscr;\n"
           "                ctx.gqr = bk.gqr; ctx.sprg = bk.sprg;\n";
    out << "                ctx.backup_stack.pop();\n";
    out << "            }\n";
    out << "            ctx.callback_depth--;\n";
    out << "            if (ctx.callback_depth <= 0) {\n";
    out << "                ctx.in_callback = false;\n";
    out << "                ctx.callback_depth = 0;\n";
    out << "            }\n";
    out << "            continue;\n";
    out << "        }\n";
    out << "        if ((ctx.pc & 0xF0000000) == 0xC0000000) ctx.pc = (ctx.pc "
           "& 0x0FFFFFFF) | 0x80000000;\n";
    out << "        else if ((ctx.pc & 0xF0000000) == 0xD0000000) ctx.pc = "
           "(ctx.pc & 0x0FFFFFFF) | 0x90000000;\n";
    out << "        else if (ctx.pc < 0x80000000) ctx.pc |= 0x80000000;\n";
    // GEN2: the dispatcher still counts one instruction per indirect
    // transfer (timing unchanged); the periodic PC print is NWII_SAMPLE
    // diagnostics only (cached flag) and no longer leaks std::hex.
    out << "        uint32_t target = ctx.pc;\n        ++ctx.inst_count;\n"
           "        if (nwii::runtime::g_env_sample && (ctx.inst_count % "
           "100000) == 0) std::cout << \"Dispatcher PC: 0x\" << std::hex << "
           "target << std::dec << std::endl;\n";
    out << "        switch (target) {\n";

    std::set<uint32_t> emitted_cases;

    func_idx = 0;
    for (const auto &[start_addr, func] : analyzer_.get_functions()) {
      std::string func_name = all_func_names[func_idx];
      const std::string hle_target =
          resolve_hle_target(start_addr, func, all_base_names[func_idx], symbols_, config_);
      bool is_hle = !hle_target.empty();
      ++func_idx;

      if (emitted_cases.insert(start_addr).second) {
        if (is_hle) {
          out << "            case 0x" << std::hex << std::uppercase
              << start_addr << std::dec << ": " << hle_target
              << "(ctx); ctx.pc = ctx.lr; break;\n";
        } else {
          out << "            case 0x" << std::hex << std::uppercase
              << start_addr << std::dec << ": " << func_name
              << "(ctx); break;\n";
        }
      }

      if (!is_hle) {
        for (const auto &inst : func.instructions) {
          ppc::Instruction ppc_inst(inst.opcode);
          if (ppc_inst.is_branch_link() || ppc_inst.opcode() == 17) {
            uint32_t ret_addr = inst.address + 4;
            if (emitted_cases.insert(ret_addr).second) {
              out << "            case 0x" << std::hex << std::uppercase
                  << ret_addr << std::dec << ": " << func_name
                  << "(ctx); break;\n";
            }
          }
          if (ppc_inst.opcode() == 16 || ppc_inst.opcode() == 18) {
            uint32_t target = ppc_inst.branch_target(inst.address);
            bool is_local =
                std::find_if(func.instructions.begin(), func.instructions.end(),
                             [target](const auto &i) {
                               return i.address == target;
                             }) != func.instructions.end();
            if (target <= inst.address && is_local) {
              if (emitted_cases.insert(inst.address).second) {
                out << "            case 0x" << std::hex << std::uppercase
                    << inst.address << std::dec << ": " << func_name
                    << "(ctx); break;\n";
              }
            }
          }
        }
        for (uint32_t jump_target : func.jump_table_targets) {
          if (emitted_cases.insert(jump_target).second) {
            out << "            case 0x" << std::hex << std::uppercase
                << jump_target << std::dec << ": " << func_name
                << "(ctx); break;\n";
          }
        }
      }
    }
    out << "            default: {\n";
    out << "                bool found = false;\n";
    // RELAOT: no entry can contain an address outside [lo, hi): skip the
    // 29k-entry scan for REL module / heap code (interpret_step, which tries
    // the REL AOT registry first, runs it exactly as before).
    out << "                if (target >= 0x" << std::hex << std::uppercase << bounds_lo
        << " && target < 0x" << bounds_hi << std::dec << ") {\n";
    // PERF3 / M2.3: O(log n) lookup of the scan's first-match row.
    out << "                    size_t lo = 0, hi = g_dispatch_seg_count;\n";
    out << "                    while (lo < hi) {\n";
    out << "                        const size_t mid = (lo + hi) >> 1;\n";
    out << "                        if (g_dispatch_segs[mid].start <= target) lo = mid + 1; else hi = mid;\n";
    out << "                    }\n";
    out << "                    if (lo != 0 && target < g_dispatch_segs[lo - 1].end) {\n";
    out << "                        ctx.pc = target;\n";
    out << "                        g_dispatch_segs[lo - 1].func(ctx);\n";
    out << "                        found = true;\n";
    out << "                    }\n";
    out << "                }\n";
    out << "                if (!found) {\n";
    out << "                    // Not in any recompiled function: this is\n";
    out << "                    // runtime-generated code (copied to low mem,\n";
    out << "                    // trampolines). Interpret it from memory.\n";
    out << "                    nwii::runtime::interpret_step(ctx);\n";
    out << "                }\n";
    out << "            }\n";
    out << "        }\n";
    out << "      } else {\n";
    out << "          continue;\n";
    out << "      }\n";
    out << "    }\n";
    out << "    std::cout << \"[DEBUG] PC History before 0x0:\\n\";\n";
    out << "    for (int i=0; i<10; ++i) {\n";
    out << "        std::cout << \"  0x\" << std::hex << "
           "pc_history[(pc_history_idx + i) % 10] << \"\\n\";\n";
    out << "    }\n";
    out << "}\n";
    out.close();
    std::cout << "[Recompiler] Generated " << main_name << "\n";
  } else {
    
    std::string fname = "output.cpp";
    generated_files.push_back(fname);
    std::ofstream out(config_.output_dir + "/" + fname);
    emit_headers(out);

    build_emitted_names();
    std::vector<std::string> all_func_names;
    const std::vector<std::string> &all_base_names = base_names_;

    out << "// --- Forward Declarations ---\n";
    out << "extern \"C\" {\n";
    out << "void OSReport(nwii::runtime::CPUContext& ctx);\n";
    out << "void IOS_Open(nwii::runtime::CPUContext& ctx);\n";
    out << "void IOS_OpenAsync(nwii::runtime::CPUContext& ctx);\n";
    out << "void IOS_Close(nwii::runtime::CPUContext& ctx);\n";
    out << "void IOS_Read(nwii::runtime::CPUContext& ctx);\n";
    out << "void IOS_Ioctl(nwii::runtime::CPUContext& ctx);\n";
    out << "void IOS_IoctlAsync(nwii::runtime::CPUContext& ctx);\n";
    out << "void IOS_Ioctlv(nwii::runtime::CPUContext& ctx);\n";
    out << "void IOS_IoctlvAsync(nwii::runtime::CPUContext& ctx);\n";
    out << "void iosAlloc(nwii::runtime::CPUContext& ctx);\n";
    out << "void iosFree(nwii::runtime::CPUContext& ctx);\n";
    out << "void VIInit(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXInit(nwii::runtime::CPUContext& ctx);\n";
    out << "void PADInit(nwii::runtime::CPUContext& ctx);\n";
    out << "void OSInit(nwii::runtime::CPUContext& ctx);\n";
    out << "void VISetBlack(nwii::runtime::CPUContext& ctx);\n";
    out << "void VIGetNextField(nwii::runtime::CPUContext& ctx);\n";
    out << "void OSDisableInterrupts(nwii::runtime::CPUContext& ctx);\n";
    out << "void OSEnableInterrupts(nwii::runtime::CPUContext& ctx);\n";
    out << "void OSRestoreInterrupts(nwii::runtime::CPUContext& ctx);\n";
    out << "void OSGetTime(nwii::runtime::CPUContext& ctx);\n";
    out << "void OSTicksToMilliseconds(nwii::runtime::CPUContext& ctx);\n";
    out << "void OSGetArenaLo(nwii::runtime::CPUContext& ctx);\n";
    out << "void OSGetArenaHi(nwii::runtime::CPUContext& ctx);\n";
    out << "void OSSetArenaLo(nwii::runtime::CPUContext& ctx);\n";
    out << "void OSSetArenaHi(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXInitFifoBase(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXInitFifoPtrs(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetCPUFifo(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXGetCPUFifo(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetCopyClear(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXBegin(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXEnd(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetVtxDesc(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetVtxAttrFmt(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXLoadPosMtxImm(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetCullMode(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetNumTevStages(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetBlendMode(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetZMode(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetZCompLoc(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetAlphaCompare(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetDstAlpha(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetScissor(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXCallDisplayList(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetCullMode(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetNumTevStages(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetBlendMode(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetZMode(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetZCompLoc(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetAlphaCompare(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetDstAlpha(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetScissor(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXLoadNrmMtxImm(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXLoadTexMtxImm(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetCurrentMtx(nwii::runtime::CPUContext& ctx);\n";
    out << "void IOS_Write(nwii::runtime::CPUContext& ctx);\n";
    out << "void IOS_Seek(nwii::runtime::CPUContext& ctx);\n";
    out << "void IOS_CloseAsync(nwii::runtime::CPUContext& ctx);\n";
    out << "void IOS_ReadAsync(nwii::runtime::CPUContext& ctx);\n";
    out << "void IOS_WriteAsync(nwii::runtime::CPUContext& ctx);\n";
    out << "void IOS_SeekAsync(nwii::runtime::CPUContext& ctx);\n";
    out << "void AXInit(nwii::runtime::CPUContext& ctx);\n";
    out << "void AXAcquireVoice(nwii::runtime::CPUContext& ctx);\n";
    out << "void AXFreeVoice(nwii::runtime::CPUContext& ctx);\n";
    out << "void AXSetVoiceState(nwii::runtime::CPUContext& ctx);\n";
    out << "void AXSetVoiceMix(nwii::runtime::CPUContext& ctx);\n";
    out << "void AXSetVoiceAdpcm(nwii::runtime::CPUContext& ctx);\n";
    out << "void AXSetVoiceSrc(nwii::runtime::CPUContext& ctx);\n";
    out << "void AXSetVoiceOffsets(nwii::runtime::CPUContext& ctx);\n";
    out << "void DVDInit(nwii::runtime::CPUContext& ctx);\n";
    out << "void DVDOpen(nwii::runtime::CPUContext& ctx);\n";
    out << "void DVDReadAsyncPrio(nwii::runtime::CPUContext& ctx);\n";
    out << "void DVDClose(nwii::runtime::CPUContext& ctx);\n";
    out << "void DVDGetDriveStatus(nwii::runtime::CPUContext& ctx);\n";
    out << "void DVDReadPrio(nwii::runtime::CPUContext& ctx);\n";
    out << "void PADRead(nwii::runtime::CPUContext& ctx);\n";
    out << "void WPADInit(nwii::runtime::CPUContext& ctx);\n";
    out << "void WPADRead(nwii::runtime::CPUContext& ctx);\n";
    out << "void KPADInit(nwii::runtime::CPUContext& ctx);\n";
    out << "void OSInitAlloc(nwii::runtime::CPUContext& ctx);\n";
    out << "void OSCreateHeap(nwii::runtime::CPUContext& ctx);\n";
    out << "void OSAllocFromHeap(nwii::runtime::CPUContext& ctx);\n";
    out << "void OSFreeToHeap(nwii::runtime::CPUContext& ctx);\n";
    out << "void OSGetCurrentThread(CPUContext& ctx);\n";
    out << "void OSSleepThread(CPUContext& ctx);\n";
    out << "}\n";
    for (const auto &[start_addr, func] : analyzer_.get_functions()) {
      const std::string &func_name = emitted_names_.at(start_addr);
      all_func_names.push_back(func_name);
      out << "void " << func_name << "(CPUContext& ctx);\n";
    }

    out << "\n// --- Function Bodies ---\n";
    int func_idx = 0;
    for (const auto &[start_addr, func] : analyzer_.get_functions()) {
      std::string func_name = all_func_names[func_idx];
      const std::string &base_name = all_base_names[func_idx++];
      const std::string target = resolve_hle_target(start_addr, func, base_name, symbols_, config_);
      if (!target.empty()) {
        if (target != func_name) {
          out << "void " << func_name << "(nwii::runtime::CPUContext& ctx) {\n";
          out << "    " << target << "(ctx);\n";
          out << "    ctx.pc = ctx.lr;\n";
          out << "}\n\n";
        }
      } else {
        emit_function(out, func, func_name);
      }
    }

    std::string entry_func = direct_target_name(entry_point);
    if (!entry_func.empty()) {
      // emitted (possibly uniquified) name of the function at the entry point
    } else if (symbols_ && symbols_->has_symbol(entry_point)) {
      entry_func = symbols_->get_symbol(entry_point);
    } else {
      std::stringstream ss;
      ss << "func_" << std::hex << std::uppercase << std::setfill('0')
         << std::setw(8) << entry_point;
      entry_func = ss.str();
    }

    // M1.11: same marker as the split export (MSR[FP] guards present).
    out << "\nstatic const bool nwii_fp_unavailable_guards =\n"
           "    (nwii::runtime::g_fp_unavailable_emulation = true);\n";

    out << "\n// --- Function Bounds Table ---\n";
    out << "struct FuncBound { uint32_t start; uint32_t end; "
           "void(*func)(CPUContext&); };\n";
    out << "static const FuncBound g_func_bounds[] = {\n";

    func_idx = 0;
    // RELAOT: hull of the table, so the dispatcher can skip the linear scan
    // for addresses no entry can contain (REL modules, heap code).
    uint32_t bounds_lo = 0xFFFFFFFFu, bounds_hi = 0;
    for (const auto &[start_addr, func] : analyzer_.get_functions()) {
      std::string func_name = all_func_names[func_idx];
      bool is_hle = !resolve_hle_target(start_addr, func, all_base_names[func_idx], symbols_, config_).empty();
      ++func_idx;
      if (!is_hle) {
        out << "    {0x" << std::hex << std::uppercase << start_addr << ", 0x"
            << func.end_address << ", " << func_name << "},\n";
        bounds_lo = std::min(bounds_lo, start_addr);
        bounds_hi = std::max(bounds_hi, func.end_address);
      }
    }
    out << "    {0, 0, nullptr}\n";
    out << "};\n\n";

    out << "// --- Entry Point Wrapper ---\n";
    out << "extern \"C\" void run_game(nwii::runtime::CPUContext& ctx) {\n";
    out << "    static uint32_t pc_history[10] = {0};\n";
    out << "    static int pc_history_idx = 0;\n";
    out << "    if (ctx.pc == 0) ctx.pc = 0x" << std::hex << std::uppercase
        << entry_point << std::dec << ";\n";
    out << "    while (ctx.is_running) {\n"
           "      pc_history[pc_history_idx] = ctx.pc;\n"
           "      pc_history_idx = (pc_history_idx + 1) % 10;\n"
           "      if (ctx.pc == 0) {\n"
           "          static bool warned_zero = false;\n"
           "          if (!warned_zero) {\n"
           "              warned_zero = true;\n"
           "              std::cout << \"[run_game] PC=0 (branch to NULL), last PCs:\";\n"
           "              for (int i = 0; i < 10; ++i)\n"
           "                  std::cout << \" 0x\" << std::hex << pc_history[(pc_history_idx + 9 - i) % 10];\n"
           "              std::cout << \" lr=0x\" << ctx.lr << std::dec << std::endl;\n"
           "          }\n"
           "          if ((ctx.msr & 0x8000) == 0) ctx.msr |= 0x8000; // Force "
           "EE=1 in idle\n"
           "          process_pending_callbacks(ctx);\n"
           "          if (ctx.pc == 0) {\n"
           "              "
           "std::this_thread::sleep_for(std::chrono::milliseconds(1));\n"
           "          }\n"
           "          continue;\n"
           "      }\n";
    out << "      if (_setjmp(ctx.exception_jmp_buf) == 0) {\n";
    out << "        process_pending_callbacks(ctx);\n";
    out << "        if (ctx.pc == 0xFFFFFFFC) {\n";
    out << "            if (!ctx.backup_stack.empty()) {\n";
    out << "                auto& bk = ctx.backup_stack.top();\n";
    out << "                ctx.gpr = bk.gpr; ctx.fpr = bk.fpr; ctx.ps1 = "
           "bk.ps1;\n";
    out << "                ctx.cr = bk.cr; ctx.lr = bk.lr; ctx.ctr = "
           "bk.ctr;\n";
    out << "                ctx.xer = bk.xer; ctx.pc = bk.pc;\n"
           "                ctx.srr0 = bk.srr0; ctx.srr1 = bk.srr1;\n"
           "                ctx.msr = bk.msr; ctx.fpscr = bk.fpscr;\n"
           "                ctx.gqr = bk.gqr; ctx.sprg = bk.sprg;\n";
    out << "                ctx.backup_stack.pop();\n";
    out << "            }\n";
    out << "            ctx.callback_depth--;\n";
    out << "            if (ctx.callback_depth <= 0) {\n";
    out << "                ctx.in_callback = false;\n";
    out << "                ctx.callback_depth = 0;\n";
    out << "            }\n";
    out << "            continue;\n";
    out << "        }\n";
    out << "        if ((ctx.pc & 0xF0000000) == 0xC0000000) ctx.pc = (ctx.pc "
           "& 0x0FFFFFFF) | 0x80000000;\n";
    out << "        else if ((ctx.pc & 0xF0000000) == 0xD0000000) ctx.pc = "
           "(ctx.pc & 0x0FFFFFFF) | 0x90000000;\n";
    out << "        else if (ctx.pc < 0x80000000) ctx.pc |= 0x80000000;\n";
    // GEN2: the dispatcher still counts one instruction per indirect
    // transfer (timing unchanged); the periodic PC print is NWII_SAMPLE
    // diagnostics only (cached flag) and no longer leaks std::hex.
    out << "        uint32_t target = ctx.pc;\n        ++ctx.inst_count;\n"
           "        if (nwii::runtime::g_env_sample && (ctx.inst_count % "
           "100000) == 0) std::cout << \"Dispatcher PC: 0x\" << std::hex << "
           "target << std::dec << std::endl;\n";
    out << "        switch (target) {\n";

    std::set<uint32_t> emitted_cases;

    func_idx = 0;
    for (const auto &[start_addr, func] : analyzer_.get_functions()) {
      std::string func_name = all_func_names[func_idx];
      const std::string hle_target =
          resolve_hle_target(start_addr, func, all_base_names[func_idx], symbols_, config_);
      bool is_hle = !hle_target.empty();
      ++func_idx;

      if (emitted_cases.insert(start_addr).second) {
        if (is_hle) {
          out << "            case 0x" << std::hex << std::uppercase
              << start_addr << std::dec << ": " << hle_target
              << "(ctx); ctx.pc = ctx.lr; break;\n";
        } else {
          out << "            case 0x" << std::hex << std::uppercase
              << start_addr << std::dec << ": " << func_name
              << "(ctx); break;\n";
        }
      }

      if (!is_hle) {
        for (const auto &inst : func.instructions) {
          ppc::Instruction ppc_inst(inst.opcode);
          if (ppc_inst.is_branch_link() || ppc_inst.opcode() == 17) {
            uint32_t ret_addr = inst.address + 4;
            if (emitted_cases.insert(ret_addr).second) {
              out << "            case 0x" << std::hex << std::uppercase
                  << ret_addr << std::dec << ": " << func_name
                  << "(ctx); break;\n";
            }
          }
          if (ppc_inst.opcode() == 16 || ppc_inst.opcode() == 18) {
            uint32_t target = ppc_inst.branch_target(inst.address);
            bool is_local =
                std::find_if(func.instructions.begin(), func.instructions.end(),
                             [target](const auto &i) {
                               return i.address == target;
                             }) != func.instructions.end();
            if (target <= inst.address && is_local) {
              if (emitted_cases.insert(inst.address).second) {
                out << "            case 0x" << std::hex << std::uppercase
                    << inst.address << std::dec << ": " << func_name
                    << "(ctx); break;\n";
              }
            }
          }
        }
        for (uint32_t jump_target : func.jump_table_targets) {
          if (emitted_cases.insert(jump_target).second) {
            out << "            case 0x" << std::hex << std::uppercase
                << jump_target << std::dec << ": " << func_name
                << "(ctx); break;\n";
          }
        }
      }
    }
    out << "            default: {\n";
    out << "                std::cerr << \"UNKNOWN DISPATCH TO 0x\" << "
           "std::hex << target << \"\\n\";\n";
    out << "                bool found = false;\n";
    out << "                for (const auto& fb : g_func_bounds) {\n";
    out << "                    if (fb.start == 0) break;\n";
    out << "                    if (target >= fb.start && target < fb.end) {\n";
    out << "                        std::cerr << \"  -> Fallback dispatch to "
           "function 0x\" << std::hex << fb.start << \"\\n\";\n";
    out << "                        ctx.pc = target;\n";
    out << "                        fb.func(ctx);\n";
    out << "                        found = true;\n";
    out << "                        break;\n";
    out << "                    }\n";
    out << "                }\n";
    out << "                if (!found) std::exit(1);\n";
    out << "            }\n";
    out << "        }\n";
    out << "      } else {\n";
    out << "          continue;\n";
    out << "      }\n";
    out << "    }\n";
    out << "    std::cout << \"[DEBUG] PC History before 0x0:\\n\";\n";
    out << "    for (int i=0; i<10; ++i) {\n";
    out << "        std::cout << \"  0x\" << std::hex << "
           "pc_history[(pc_history_idx + i) % 10] << \"\\n\";\n";
    out << "    }\n";
    out << "}\n";
  }

  // Audit trail (not compiled): every function whose body is replaced by a
  // native handler, with the reason, so a seam binding can be reviewed and
  // snapshot-tested without grepping the dispatcher.
  {
    std::ofstream report(config_.output_dir + "/seam_bindings.csv");
    report << "address,symbol,handler,source\n";
    size_t idx = 0;
    size_t bound = 0;
    for (const auto &[start_addr, func] : analyzer_.get_functions()) {
      const std::string &base = base_names_[idx++];
      const std::string target =
          resolve_hle_target(start_addr, func, base, symbols_, config_);
      if (target.empty())
        continue;
      ++bound;
      const char *source = config_.hle_hooks.count(start_addr) ? "hle_hooks"
                           : (symbols_ && symbols_->has_authoritative_symbol(start_addr))
                               ? "symbol"
                               : (!func.hle_hook_name.empty() ? "signature" : "symbol");
      report << "0x" << std::hex << std::uppercase << std::setw(8)
             << std::setfill('0') << start_addr << std::dec << std::setfill(' ')
             << "," << emitted_names_.at(start_addr) << "," << target << ","
             << source << "\n";
    }
    std::cout << "[Recompiler] Seam-bound functions: " << bound
              << " (see seam_bindings.csv)\n";
  }
  std::cout << "[Recompiler] Direct calls/branches by name: " << direct_calls_
            << ", non-local branches left to the dispatcher (no function at "
               "target): "
            << dispatcher_branches_ << "\n";

  return generated_files;
}

bool Recompiler::generate_cmake_project(uint32_t entry_point) {
  if (!std::filesystem::exists(config_.output_dir)) {
    std::filesystem::create_directories(config_.output_dir);
  }

  std::vector<std::string> generated_files = generate_cpp(entry_point);
  if (generated_files.empty()) {
    return false;
  }

  // Copy the runtime sources, and only the sources.
  //
  // A plain recursive copy takes the source tree's own `build` directory with
  // it — CMake caches, object files, fetched dependencies and their `.git`
  // packs. Those are read-only inside git object stores, so the copy dies on
  // the first pack file and the whole export fails. It also happens to be the
  // last step, so everything generated up to that point was silently thrown
  // away, including the SDK symbol report.
  std::string runtime_dest = config_.output_dir + "/nWiiRuntime";
  try {
    namespace fs = std::filesystem;
    const fs::path src = config_.runtime_source_dir;
    const auto opts = fs::copy_options::overwrite_existing;

    auto skip = [](const fs::path &rel) {
      for (const auto &part : rel)
      {
        const std::string s = part.string();
        if (s == "build" || s == ".git" || s == ".cache" || s == ".DS_Store")
          return true;
      }
      return false;
    };

    fs::create_directories(runtime_dest);
    for (auto it = fs::recursive_directory_iterator(src);
         it != fs::recursive_directory_iterator(); ++it)
    {
      const fs::path rel = fs::relative(it->path(), src);
      if (skip(rel))
      {
        if (it->is_directory()) it.disable_recursion_pending();
        continue;
      }
      const fs::path dest = fs::path(runtime_dest) / rel;
      if (it->is_directory())
        fs::create_directories(dest);
      else if (it->is_regular_file())
        fs::copy_file(it->path(), dest, opts);
    }
  } catch (const std::exception &e) {
    std::cerr << "Failed to copy runtime from " << config_.runtime_source_dir
              << ": " << e.what() << "\n";
    return false;
  }

  std::string cmake_path = config_.output_dir + "/CMakeLists.txt";
  std::ofstream out(cmake_path);
  if (!out.is_open())
    return false;

  out << "cmake_minimum_required(VERSION 3.20)\n";

  
  out << "project(" << config_.project_name << " LANGUAGES C CXX)\n\n";
  out << "set(CMAKE_POLICY_VERSION_MINIMUM 3.5)\n";
  out << "set(CMAKE_CXX_STANDARD 20)\n";
  out << "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n\n";

  
  
  out << "include(FetchContent)\n";
  out << "FetchContent_Declare(\n";
  out << "    tomlplusplus\n";
  out << "    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git\n";
  out << "    GIT_TAG        v3.4.0\n";
  out << ")\n";
  out << "FetchContent_MakeAvailable(tomlplusplus)\n\n";
  out << "FetchContent_Declare(\n";
  out << "    SDL2\n";
  out << "    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git\n";
  out << "    GIT_TAG        release-2.28.5\n";
  out << ")\n";
  out << "set(SDL2_DISABLE_INSTALL ON CACHE BOOL \"\" FORCE)\n";
  out << "FetchContent_MakeAvailable(SDL2)\n\n";

  

  out << "if(APPLE)\n";
  out << "    set(_sdl_gl_src "
         "\"${sdl2_SOURCE_DIR}/src/video/cocoa/SDL_cocoaopengl.m\")\n";
  out << "    if(EXISTS \"${_sdl_gl_src}\")\n";
  out << "        file(READ \"${_sdl_gl_src}\" _sdl_gl_txt)\n";
  out << "        string(REPLACE \"attr[i++] = "
         "NSOpenGLPFAAllowOfflineRenderers;\"\n";
  out << "                       \"/* NWii: breaks pixel format on dual-GPU "
         "Macs */\"\n";
  out << "                       _sdl_gl_patched \"${_sdl_gl_txt}\")\n";
  out << "        if(NOT _sdl_gl_txt STREQUAL _sdl_gl_patched)\n";
  out << "            file(WRITE \"${_sdl_gl_src}\" \"${_sdl_gl_patched}\")\n";
  out << "        endif()\n";
  out << "    endif()\n";
  out << "endif()\n\n";

  out << "add_subdirectory(nWiiRuntime)\n\n";

  out << "add_executable(" << config_.project_name
      << " nWiiRuntime/src/core/main.cpp";
  for (const auto &file : generated_files) {
    out << " " << file;
  }
  out << ")\n";

  out << "target_include_directories(" << config_.project_name
      << " PRIVATE nWiiRuntime/src nWiiRuntime/include)\n";

  
  out << "target_link_libraries(" << config_.project_name
      << " PRIVATE nwiiruntime)\n";

  return true;
}

std::string Recompiler::generate_function_cpp(const analyzer::Function &func) {
  std::stringstream ss;

  std::string func_name;
  if (symbols_ && symbols_->has_symbol(func.start_address)) {
    func_name = symbols_->get_symbol(func.start_address);
  } else {
    std::stringstream ss_name;
    ss_name << "func_" << std::hex << std::uppercase << std::setfill('0')
            << std::setw(8) << func.start_address;
    func_name = ss_name.str();
  }

  const auto hook = config_.hle_hooks.find(func.start_address);
  if (hook != config_.hle_hooks.end()) {
    ss << "void " << func_name << "(CPUContext& ctx) {\n";
    ss << "    " << hook->second << "(ctx);\n";
    ss << "    ctx.pc = ctx.lr; return;\n}\n\n";
  } else {
    emit_function(ss, func, func_name);
  }
  return ss.str();
}

// PERF3 (roadmap M2.9 fastmem): the MMU access wrappers are force-inlined
// (cpu_context.h). MSVC's /O1 optimizer time grows superlinearly with the
// blocks of one function, and every instruction of a generated function is
// already a switch target, so a few hundred inlined accesses in one function
// made a full rebuild 2.6x slower (output_146: 17 s -> 137 s). Functions with
// more than kOutlineMemSites (200) accesses (278 of 29.5k in SOUE01, 21% of the
// access sites, mostly one-shot initialisers) call the out-of-line _ol forms
// instead: same semantics (they wrap the same inline code), one call each.
void Recompiler::outline_memory_accesses(std::string &fn_text) {
  static const char *const kOps[] = {"read8(",   "read16(",    "read32(",    "read64(",
                                     "read_f32(", "read_f64(", "write8(",    "write16(",
                                     "write32(",  "write64(",  "write_f32(", "write_f64("};
  const std::string prefix = "ctx.mmu.";
  size_t sites = 0;
  for (size_t p = fn_text.find(prefix); p != std::string::npos; p = fn_text.find(prefix, p + 1))
    ++sites;
  if (sites <= kOutlineMemSites)
    return;
  std::string outp;
  outp.reserve(fn_text.size() + sites * 3);
  size_t pos = 0;
  for (size_t p = fn_text.find(prefix); p != std::string::npos; p = fn_text.find(prefix, p + 1)) {
    const size_t q = p + prefix.size();
    for (const char *op : kOps) {
      const size_t n = std::strlen(op);
      if (fn_text.compare(q, n, op) == 0) {
        outp.append(fn_text, pos, q + n - 1 - pos);  // up to the name, without '('
        outp += "_ol(";
        pos = q + n;
        break;
      }
    }
  }
  outp.append(fn_text, pos, std::string::npos);
  fn_text.swap(outp);
}

void Recompiler::emit_function(std::ostream &out,
                               const analyzer::Function &func,
                               const std::string &func_name) {
  std::ostringstream body;
  emit_function_body(body, func, func_name);
  std::string text = body.str();
  outline_memory_accesses(text);
  out << text;
}

void Recompiler::emit_function_body(std::ostream &out,
                                    const analyzer::Function &func,
                                    const std::string &func_name) {
  out << "void " << func_name << "(CPUContext& ctx) {\n";

  
  
  

  
  
  bool all_zero = !func.instructions.empty();
  for (const auto &inst : func.instructions) {
    if (inst.opcode != 0) {
      all_zero = false;
      break;
    }
  }
  bool os_low_mem = (func.start_address & 0x3FFFFFFF) < 0x3100;
  if (all_zero || os_low_mem) {
    out << "    nwii::runtime::interpret_step(ctx);\n";
    out << "}\n\n";
    return;
  }

  
  out << "    if (nwii::runtime::g_trace_calls && ctx.pc == 0x" << std::hex
      << std::uppercase << func.start_address << std::dec
      << ") nwii::runtime::trace_call(0x" << std::hex << std::uppercase
      << func.start_address << std::dec << ", ctx);\n";

  std::set<uint32_t> valid_addrs;
  for (const auto &inst : func.instructions) {
    valid_addrs.insert(inst.address);
  }

  if (valid_addrs.size() > 1) {
    out << "    if (ctx.pc != 0x" << std::hex << std::uppercase
        << func.start_address << std::dec << ") {\n";
    out << "        switch (ctx.pc) {\n";
    for (uint32_t addr : valid_addrs) {
      if (addr != func.start_address) {
        out << "            case 0x" << std::hex << std::uppercase << addr
            << std::dec << ": goto loc_" << std::hex << std::uppercase << addr
            << std::dec << ";\n";
      }
    }
    out << "            case 0x0: return; // idle: no runnable thread\n";

    
    
    out << "            default: nwii::runtime::interpret_step(ctx); return;\n";
    out << "        }\n";
    out << "    }\n";

  }
  for (const auto &inst : func.instructions) {

    

    
    out << "    ++ctx.inst_count;\n";
    emit_instruction(out, inst, func);
  }

  out << "}\n\n";
}

void Recompiler::emit_instruction(std::ostream &out,
                                  const analyzer::Instruction &inst,
                                  const analyzer::Function &func) {
  ppc::Instruction ppc_inst(inst.opcode);

  out << "loc_" << std::hex << std::uppercase << inst.address << std::dec
      << ": ;\n";
  out << "    // 0x" << std::hex << std::setfill('0') << std::setw(8)
      << inst.address << ": ";
  out << std::setfill('0') << std::setw(8) << inst.opcode << std::dec << "\n";

  // RELAOT: a word covered by a REL relocation. OSLink has patched it in
  // guest RAM; the patched immediate / branch displacement is read from there
  // at run time (SH = host pointer of the section base).
  struct ImmOverrideReset {
    bool &flag;
    ~ImmOverrideReset() { flag = false; }
  } imm_override_reset{imm_override_};
  if (rel_) {
    const uint32_t off = inst.address - rel_->va_base;
    const auto rit = rel_->relocs.find(off);
    if (rit != rel_->relocs.end()) {
      const uint32_t op = ppc_inst.opcode();
      if (op == 18) {
        if (emit_rel_branch(out, inst, rit->second))
          return;
      } else if (op == 12 || op == 13 || op == 14 || op == 15 ||
                 (op >= 32 && op <= 45) || op == 48 || op == 50 || op == 52 ||
                 op == 54) {
        out << "    RIMM = nwii::runtime::relaot::rd_simm(SH + 0x" << std::hex
            << std::uppercase << off << std::dec << "u);\n";
        imm_override_ = true;
        ++rel_->n_reloc_imm;
      } else {
        // Any other relocated form: execute the patched word itself.
        ++rel_->n_reloc_micro;
        out << "    nwii::runtime::micro_interpret(ctx, "
               "nwii::runtime::relaot::rd32(SH + 0x"
            << std::hex << std::uppercase << off << std::dec << "u), "
            << pcl(inst.address) << ");\n";
        out << "    if (ctx.pc != " << pcl(inst.address + 4) << ") return;\n";
        return;
      }
    }
  }

  if (ppc_inst.opcode() == 14) {
    
    uint32_t rD = ppc_inst.rd();
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();

    if (rA == 0) {
      out << "    ctx.gpr[" << rD << "] = " << imm_s(simm) << "; // li r" << rD << ", "
          << simm << "\n";
    } else {
      out << "    ctx.gpr[" << rD << "] = ctx.gpr[" << rA << "] + " << imm_s(simm)
          << "; // addi r" << rD << ", r" << rA << ", " << simm << "\n";
    }
  } else if (ppc_inst.opcode() == 31 && ppc_inst.extended_opcode() == 444) {
    
    uint32_t rS = ppc_inst.rs();
    uint32_t rA = ppc_inst.ra();
    uint32_t rB = ppc_inst.rb();
    if (rS == rB) {
      out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rS << "]; // mr r" << rA
          << ", r" << rS << "\n";
    } else {
      out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rS << "] | ctx.gpr["
          << rB << "]; // or r" << rA << ", r" << rS << ", r" << rB << "\n";
    }

    
    if (ppc_inst.value() & 1) {
      emit_cr0(out, rA);
    }
  } else if (ppc_inst.opcode() == 31 && ppc_inst.extended_opcode() == 339) {
    
    uint32_t rD = ppc_inst.rd();
    uint32_t spr_bottom = (ppc_inst.value() >> 16) & 0x1F;
    uint32_t spr_top = (ppc_inst.value() >> 11) & 0x1F;
    uint32_t spr = (spr_top << 5) | spr_bottom;
    if (spr == 8) {
      out << "    ctx.gpr[" << rD << "] = ctx.lr; // mflr r" << rD << "\n";
    } else if (spr == 9) {
      out << "    ctx.gpr[" << rD << "] = ctx.ctr; // mfctr r" << rD << "\n";
    } else if (spr == 26) {
      out << "    ctx.gpr[" << rD << "] = ctx.srr0; // mfsrr0 r" << rD << "\n";
    } else if (spr == 27) {
      out << "    ctx.gpr[" << rD << "] = ctx.srr1; // mfsrr1 r" << rD << "\n";
    } else if (spr == 22) {
      out << "    ctx.gpr[" << rD << "] = ctx.read_dec(); // mfdec r" << rD << "\n";
    } else if (spr >= 912 && spr <= 919) {
      out << "    ctx.gpr[" << rD << "] = ctx.gqr[" << (spr - 912)
          << "]; // mfgqr" << (spr - 912) << " r" << rD << "\n";
    } else if (spr == 1) {
      out << "    ctx.gpr[" << rD << "] = ctx.xer; // mfxer r" << rD << "\n";
    } else if (spr == 920 || spr == 922 || spr == 923) {
      out << "    ctx.gpr[" << rD << "] = ctx.read_lc_spr(" << spr
          << "); // mfspr locked cache\n";
    } else if (spr == 921 || spr == 924) {
      out << "    ctx.gpr[" << rD << "] = 0; // mfspr HID " << spr << "\n";
    } else if (spr == 1008) {
      out << "    ctx.gpr[" << rD << "] = 0; // mfspr HID0 (stub)\n";
    } else if (spr == 287) {
      out << "    ctx.gpr[" << rD
          << "] = (nwii::runtime::Config::get().platform == "
             "nwii::runtime::Platform::GameCube) ? 0x00083214 : 0x00087102; // "
             "mfspr PVR\n";
    } else if (spr == 1017) {
      out << "    ctx.gpr[" << rD << "] = 0; // mfspr L2CR\n";
    } else {
      out << "    std::cerr << \"[WARN] mfspr r\" << " << rD
          << " << \" spr=\" << " << spr << " << \" (stub=0)\\n\";\n";
      out << "    ctx.gpr[" << rD << "] = 0; // unknown spr stub\n";
    }
  } else if (ppc_inst.opcode() == 31 && ppc_inst.extended_opcode() == 467) {
    
    uint32_t rS = ppc_inst.rs();
    uint32_t spr_bottom = (ppc_inst.value() >> 16) & 0x1F;
    uint32_t spr_top = (ppc_inst.value() >> 11) & 0x1F;
    uint32_t spr = (spr_top << 5) | spr_bottom;
    if (spr == 8) {
      out << "    ctx.lr = ctx.gpr[" << rS << "]; // mtlr r" << rS << "\n";
    } else if (spr == 9) {
      out << "    ctx.ctr = ctx.gpr[" << rS << "]; // mtctr r" << rS << "\n";
    } else if (spr == 26) {
      out << "    ctx.srr0 = ctx.gpr[" << rS << "]; // mtsrr0 r" << rS << "\n";
    } else if (spr == 27) {
      out << "    ctx.srr1 = ctx.gpr[" << rS << "]; // mtsrr1 r" << rS << "\n";
    } else if (spr == 22) {
      out << "    ctx.write_dec(ctx.gpr[" << rS << "]); // mtdec r" << rS << "\n";
    } else if (spr >= 912 && spr <= 919) {
      out << "    ctx.gqr[" << (spr - 912) << "] = ctx.gpr[" << rS
          << "]; // mtgqr" << (spr - 912) << " r" << rS << "\n";
    } else if (spr == 1) {
      out << "    ctx.xer = ctx.gpr[" << rS << "]; // mtxer r" << rS << "\n";
    } else if (spr == 920 || spr == 922 || spr == 923) {
      out << "    ctx.write_lc_spr(" << spr << ", ctx.gpr[" << rS
          << "]); // mtspr locked cache\n";
    } else {
      out << "    // unhandled mtspr " << spr
          << "\n"; 
    }
  } else if (ppc_inst.opcode() == 31 && ppc_inst.extended_opcode() == 0) {
    
    uint32_t crD = ppc_inst.rd() >> 2; 
    uint32_t rA = ppc_inst.ra();
    uint32_t rB = ppc_inst.rb();

    out << "    ctx.cr[" << crD << "].lt = ((int32_t)ctx.gpr[" << rA
        << "] < (int32_t)ctx.gpr[" << rB << "]);\n";
    out << "    ctx.cr[" << crD << "].gt = ((int32_t)ctx.gpr[" << rA
        << "] > (int32_t)ctx.gpr[" << rB << "]);\n";
    out << "    ctx.cr[" << crD << "].eq = ((int32_t)ctx.gpr[" << rA
        << "] == (int32_t)ctx.gpr[" << rB << "]);\n";
    out << "    ctx.cr[" << crD << "].so = (ctx.xer >> 31) & 1;\n";
  } else if (ppc_inst.opcode() == 32) {
    
    uint32_t rD = ppc_inst.rd();
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    if (rA == 0) {
      out << "    ctx.gpr[" << rD << "] = ctx.mmu.read32(" << imm_s(simm)
          << "); // lwz r" << rD << ", " << simm << "(0)\n";
    } else {
      out << "    ctx.gpr[" << rD << "] = ctx.mmu.read32(ctx.gpr[" << rA
          << "] + " << imm_s(simm) << "); // lwz r" << rD << ", " << simm << "(r" << rA
          << ")\n";
    }
  } else if (ppc_inst.opcode() == 33) {
    
    uint32_t rD = ppc_inst.rd();
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rA << "] + " << imm_s(simm)
        << ";\n";
    out << "    ctx.gpr[" << rD << "] = ctx.mmu.read32(ctx.gpr[" << rA
        << "]); // lwzu r" << rD << ", " << simm << "(r" << rA << ")\n";
  } else if (ppc_inst.opcode() == 34) {
    
    uint32_t rD = ppc_inst.rd();
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    if (rA == 0) {
      out << "    ctx.gpr[" << rD << "] = ctx.mmu.read8(" << imm_s(simm)
          << "); // lbz r" << rD << ", " << simm << "(0)\n";
    } else {
      out << "    ctx.gpr[" << rD << "] = ctx.mmu.read8(ctx.gpr[" << rA
          << "] + " << imm_s(simm) << "); // lbz r" << rD << ", " << simm << "(r" << rA
          << ")\n";
    }
  } else if (ppc_inst.opcode() == 35) {
    
    uint32_t rD = ppc_inst.rd();
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rA << "] + " << imm_s(simm)
        << ";\n";
    out << "    ctx.gpr[" << rD << "] = ctx.mmu.read8(ctx.gpr[" << rA
        << "]); // lbzu r" << rD << ", " << simm << "(r" << rA << ")\n";
  } else if (ppc_inst.opcode() == 36) {
    
    uint32_t rS = ppc_inst.rs();
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    if (rA == 0) {
      out << "    ctx.mmu.write32(" << imm_s(simm) << ", ctx.gpr[" << rS
          << "]); // stw r" << rS << ", " << simm << "(0)\n";
    } else {
      out << "    ctx.mmu.write32(ctx.gpr[" << rA << "] + " << imm_s(simm)
          << ", ctx.gpr[" << rS << "]); // stw r" << rS << ", " << simm << "(r"
          << rA << ")\n";
    }
  } else if (ppc_inst.opcode() == 37) {
    
    uint32_t rS = ppc_inst.rs();
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    out << "    {\n";
    out << "        uint32_t ea = ctx.gpr[" << rA << "] + " << imm_s(simm) << ";\n";
    out << "        ctx.mmu.write32(ea, ctx.gpr[" << rS << "]);\n";
    out << "        ctx.gpr[" << rA << "] = ea;\n";
    out << "    } // stwu r" << rS << ", " << simm << "(r" << rA << ")\n";
  } else if (ppc_inst.opcode() == 38) {
    
    uint32_t rS = ppc_inst.rs();
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    if (rA == 0) {
      out << "    ctx.mmu.write8(" << imm_s(simm) << ", (uint8_t)ctx.gpr[" << rS
          << "]); // stb r" << rS << ", " << simm << "(0)\n";
    } else {
      out << "    ctx.mmu.write8(ctx.gpr[" << rA << "] + " << imm_s(simm)
          << ", (uint8_t)ctx.gpr[" << rS << "]); // stb r" << rS << ", " << simm
          << "(r" << rA << ")\n";
    }
  } else if (ppc_inst.opcode() == 39) {
    
    uint32_t rS = ppc_inst.rs();
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    out << "    {\n";
    out << "        uint32_t ea = ctx.gpr[" << rA << "] + " << imm_s(simm) << ";\n";
    out << "        ctx.mmu.write8(ea, (uint8_t)ctx.gpr[" << rS << "]);\n";
    out << "        ctx.gpr[" << rA << "] = ea;\n";
    out << "    } // stbu r" << rS << ", " << simm << "(r" << rA << ")\n";
  } else if (ppc_inst.opcode() == 16) {
    
    uint32_t bo = ppc_inst.bo();
    uint32_t bi = ppc_inst.bi();
    uint32_t cr_idx = bi / 4;
    uint32_t cr_bit = bi % 4;

    std::string cond_field;
    if (cr_bit == 0)
      cond_field = "lt";
    else if (cr_bit == 1)
      cond_field = "gt";
    else if (cr_bit == 2)
      cond_field = "eq";
    else
      cond_field = "so";

    int16_t bd = ppc_inst.simm() & 0xFFFC; 
    int32_t target = bd;                   
    if ((inst.opcode & 2) == 2) {          
      target = bd;                         
      out << "    // Absolute branches not fully supported in simple string "
             "emit\n";
    } else {
      target = inst.address + bd;
    }

    std::stringstream ss;
    ss << "loc_" << std::hex << std::uppercase << target;
    std::string target_lbl = ss.str();

    bool is_local =
        !has_hle_target(target, analyzer_, symbols_, config_) &&
        std::find_if(func.instructions.begin(), func.instructions.end(),
                     [target](const auto &i) { return i.address == target; }) !=
        func.instructions.end();

    if (target <= inst.address && is_local) {

      out << "    ctx.pc = " << pcl(inst.address) << ";\n";
      out << "    if (process_pending_callbacks(ctx)) return;\n";
      // GEN2: diagnostics only under NWII_SAMPLE (cached flag); without it
      // the back-edge pays no 64-bit modulo.
      if (!rel_) out << "    if (nwii::runtime::g_env_sample && (ctx.inst_count % 10000000) == 0) { std::cout << "
             "\"Spinning at PC: 0x\" << std::hex << 0x"
          << std::uppercase << std::hex << inst.address << std::dec
          << " << \" LR: 0x\" << std::hex << ctx.lr << std::dec << std::endl; "
             "}\n";
    }

    // Resolve by address through the uniquified name map: a sanitised symbol
    // can name several functions, and only the address picks the right one.
    const std::string ext_name = is_local ? std::string() : direct_target_name(target);

    bool bit0 = (bo & 0x10) != 0;
    bool bit1 = (bo & 0x08) != 0;
    bool bit2 = (bo & 0x04) != 0;
    bool bit3 = (bo & 0x02) != 0;

    bool dec_ctr = !bit2;
    bool test_ctr = !bit2;
    bool test_cr = !bit0;

    std::vector<std::string> conditions;
    if (test_ctr) {
      if (bit3)
        conditions.push_back("ctx.ctr == 0");
      else
        conditions.push_back("ctx.ctr != 0");
    }
    if (test_cr) {
      if (bit1)
        conditions.push_back("ctx.cr[" + std::to_string(cr_idx) + "]." +
                             cond_field);
      else
        conditions.push_back("!ctx.cr[" + std::to_string(cr_idx) + "]." +
                             cond_field);
    }

    std::string cond_expr;
    if (conditions.empty())
      cond_expr = "true";
    else if (conditions.size() == 1)
      cond_expr = conditions[0];
    else
      cond_expr = conditions[0] + " && " + conditions[1];

    if (dec_ctr)
      out << "    ctx.ctr--;\n";

    if (ppc_inst.lk())
      out << "    ctx.lr = " << pcl(inst.address + 4) << "; // save return address\n";

    if (is_local) {
      if (cond_expr == "true")
        out << "    goto " << target_lbl << ";\n";
      else
        out << "    if (" << cond_expr << ") goto " << target_lbl << ";\n";
    } else {
      const bool is_mapped = !ext_name.empty();
      ++(is_mapped ? direct_calls_ : dispatcher_branches_);
      if (is_mapped) {
        if (cond_expr == "true") {
          out << "    ctx.pc = " << pcl(target) << "; " << ext_name << "(ctx); return;\n";
        } else {
          out << "    if (" << cond_expr << ") { ctx.pc = " << pcl(target) << "; " << ext_name
              << "(ctx); return; }\n";
        }
      } else {
        if (cond_expr == "true") {
          out << "    ctx.pc = " << pcl(target, false) << "; return;\n";
        } else {
          out << "    if (" << cond_expr << ") { ctx.pc = " << pcl(target, false) << "; return; }\n";
        }
      }
    }
  } else if (ppc_inst.opcode() == 18) {
    
    uint32_t target = ppc_inst.branch_target(inst.address);
    std::stringstream ss;
    std::string target_name;

    // The analyzer may include a backward branch target in this function's
    // instruction range. Native hooks still take precedence at that address.
    bool is_local =
        !has_hle_target(target, analyzer_, symbols_, config_) &&
        std::find_if(func.instructions.begin(), func.instructions.end(),
                     [target](const auto &i) { return i.address == target; }) !=
        func.instructions.end();

    if (target <= inst.address && is_local) {

      out << "    ctx.pc = " << pcl(inst.address) << ";\n";
      out << "    if (process_pending_callbacks(ctx)) return;\n";
      // GEN2: diagnostics only under NWII_SAMPLE (cached flag); without it
      // the back-edge pays no 64-bit modulo.
      if (!rel_) out << "    if (nwii::runtime::g_env_sample && (ctx.inst_count % 10000000) == 0) { std::cout << "
             "\"Spinning at PC: 0x\" << std::hex << 0x"
          << std::uppercase << std::hex << inst.address << std::dec
          << " << \" LR: 0x\" << std::hex << ctx.lr << std::dec << std::endl; "
             "}\n";
    }

    // Address -> emitted (uniquified) name; "" when no function starts there.
    if (!is_local)
      target_name = direct_target_name(target);

    if (ppc_inst.is_branch_link()) {
      out << "    ctx.lr = " << pcl(inst.address + 4) << "; // save return address\n";
      if (is_local) {
        out << "    goto loc_" << std::hex << std::uppercase
            << std::setfill('0') << std::setw(8) << target << ";\n";
      } else {
        const bool is_mapped = !target_name.empty();
        ++(is_mapped ? direct_calls_ : dispatcher_branches_);
        if (is_mapped) {
          out << "    ctx.pc = " << pcl(target) << "; ";
          out << target_name << "(ctx);\n";
          out << "    if (ctx.pc != " << pcl(inst.address + 4) << ") return;\n";
        } else {
          out << "    ctx.pc = " << pcl(target, false) << "; return; // bl to unmapped target\n";
        }
      }
    } else {
      if (is_local) {
        out << "    goto loc_" << std::hex << std::uppercase
            << std::setfill('0') << std::setw(8) << target << ";\n";
      } else {
        // Tail branch: always through the dispatcher (no native call).
        if (!target_name.empty())
          out << "    ctx.pc = " << pcl(target) << "; return;\n";
        else
          out << "    ctx.pc = " << pcl(target, false) << "; return;\n";
      }
    }
  } else if (ppc_inst.opcode() == 19) {
    uint32_t xo = ppc_inst.extended_opcode();
    if (xo == 150) { 
      out << "    // isync\n";
    } else if (xo == 50) { 
        out << "    {\n";
        out << "        uint32_t ee_was = ctx.msr & 0x8000;\n";
        out << "        ctx.msr = ctx.srr1; // rfi: restore MSR from SRR1\n";
        out << "        ctx.pc = ctx.srr0;  // rfi: jump to SRR0\n";
        out << "        ctx.dispatch_saved_ctx = 0; // exit ISR\n";
        // M1.11: FP Unavailable when MSR[FP]=0 (lazy FPU switching of the
        // OS), taken before any interrupt can save the new context.
        out << "        ctx.fp_unavailable_check();\n";
        out << "        if (!ee_was && (ctx.msr & 0x8000)) {\n";
        out << "            if (process_pending_callbacks(ctx)) return;\n";
        out << "        }\n";
        out << "    }\n";
        out << "    return;\n";
    } else if (xo == 16 || xo == 528) { 
      uint32_t bo = ppc_inst.bo();
      uint32_t bi = ppc_inst.bi();
      uint32_t cr_idx = bi / 4;
      uint32_t cr_bit = bi % 4;

      std::string cond_field;
      if (cr_bit == 0)
        cond_field = "lt";
      else if (cr_bit == 1)
        cond_field = "gt";
      else if (cr_bit == 2)
        cond_field = "eq";
      else
        cond_field = "so";

      std::string target_reg = (xo == 16) ? "ctx.lr" : "ctx.ctr";
      // RELAOT: REL code masks the target like the interpreter (and Gekko)
      const std::string tgt = rel_ ? "(" + target_reg + " & ~3u)" : target_reg;

      bool bit0 = (bo & 0x10) != 0;
      bool bit1 = (bo & 0x08) != 0;
      bool bit2 = (bo & 0x04) != 0;
      bool bit3 = (bo & 0x02) != 0;

      bool dec_ctr = !bit2;
      bool test_ctr = !bit2;
      bool test_cr = !bit0;

      std::vector<std::string> conditions;
      if (test_ctr) {
        if (bit3)
          conditions.push_back("ctx.ctr == 0");
        else
          conditions.push_back("ctx.ctr != 0");
      }
      if (test_cr) {
        if (bit1)
          conditions.push_back("ctx.cr[" + std::to_string(cr_idx) + "]." +
                               cond_field);
        else
          conditions.push_back("!ctx.cr[" + std::to_string(cr_idx) + "]." +
                               cond_field);
      }

      std::string cond_expr;
      if (conditions.empty())
        cond_expr = "true";
      else if (conditions.size() == 1)
        cond_expr = conditions[0];
      else
        cond_expr = conditions[0] + " && " + conditions[1];

      if (dec_ctr)
        out << "    ctx.ctr--;\n";

      if (ppc_inst.lk() && target_reg == "ctx.lr") {
        out << "    {\n";
        out << "        uint32_t temp_lr = ctx.lr;\n";
        out << "        ctx.lr = " << pcl(inst.address + 4) << "; // save return address\n";
        if (cond_expr == "true") {
          out << "        ctx.pc = " << (rel_ ? "(temp_lr & ~3u)" : "temp_lr") << "; return;\n";
        } else {
          out << "        if (" << cond_expr
              << ") { ctx.pc = " << (rel_ ? "(temp_lr & ~3u)" : "temp_lr") << "; return; }\n";
        }
        out << "    }\n";
      } else {
        if (ppc_inst.lk()) {
          out << "    ctx.lr = " << pcl(inst.address + 4) << "; // save return address\n";
        }

        
        bool null_guard = ppc_inst.lk() && target_reg == "ctx.ctr" && !rel_;
        if (cond_expr == "true") {
          if (null_guard)
            out << "    if (" << target_reg << ") { ctx.pc = " << target_reg
                << "; return; } else nwii::runtime::note_null_call("
                << pcl(inst.address)
                << ", ctx.lr);\n";
          else
            out << "    ctx.pc = " << tgt << "; return;\n";
        } else {
          if (null_guard)
            out << "    if ((" << cond_expr << ") && " << target_reg
                << ") { ctx.pc = " << target_reg << "; return; }\n";
          else
            out << "    if (" << cond_expr << ") { ctx.pc = " << tgt
                << "; return; }\n";
        }
      }
    } else if (xo == 257 || xo == 129 || xo == 289 || xo == 225 || xo == 33 ||
               xo == 449 || xo == 417 || xo == 193) {
      uint32_t crbD = ppc_inst.rd();
      uint32_t crbA = ppc_inst.ra();
      uint32_t crbB = ppc_inst.rb();

      auto get_cr_bit_str = [](uint32_t bit_idx) {
        std::string field = "ctx.cr[" + std::to_string(bit_idx / 4) + "]";
        uint32_t bit = bit_idx % 4;
        if (bit == 0)
          return field + ".lt";
        if (bit == 1)
          return field + ".gt";
        if (bit == 2)
          return field + ".eq";
        return field + ".so";
      };

      std::string op;
      if (xo == 257)
        op = get_cr_bit_str(crbA) + " & " + get_cr_bit_str(crbB); 
      else if (xo == 129)
        op = get_cr_bit_str(crbA) + " & !" + get_cr_bit_str(crbB); 
      else if (xo == 289)
        op = get_cr_bit_str(crbA) + " == " + get_cr_bit_str(crbB); 
      else if (xo == 225)
        op = "!(" + get_cr_bit_str(crbA) + " & " + get_cr_bit_str(crbB) +
             ")"; 
      else if (xo == 33)
        op = "!(" + get_cr_bit_str(crbA) + " | " + get_cr_bit_str(crbB) +
             ")"; 
      else if (xo == 449)
        op = get_cr_bit_str(crbA) + " | " + get_cr_bit_str(crbB); 
      else if (xo == 417)
        op = get_cr_bit_str(crbA) + " | !" + get_cr_bit_str(crbB); 
      else if (xo == 193)
        op = get_cr_bit_str(crbA) + " ^ " + get_cr_bit_str(crbB); 

      out << "    " << get_cr_bit_str(crbD) << " = " << op << ";\n";
    } else if (xo == 0) {
      uint32_t crfD = ppc_inst.rd() >> 2;
      uint32_t crfS = ppc_inst.ra() >> 2;
      out << "    ctx.cr[" << crfD << "] = ctx.cr[" << crfS << "]; // mcrf\n";
    } else {
      out << "    std::cerr << \"UNIMPLEMENTED OPCODE 19 XO \" << " << xo
          << " << \" at 0x" << std::hex << inst.address << std::dec
          << "\\n\"; std::exit(1);\n";
    }
  } else if (ppc_inst.opcode() == 48) {
    
    uint32_t fD = ppc_inst.rd();
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();

    
    
    if (rA == 0) {
      out << "    ctx.fpr[" << fD << "] = ctx.ps1[" << fD
          << "] = ctx.mmu.read_f32(" << imm_s(simm) << "); // lfs\n";
    } else {
      out << "    ctx.fpr[" << fD << "] = ctx.ps1[" << fD
          << "] = ctx.mmu.read_f32(ctx.gpr[" << rA << "] + " << imm_s(simm)
          << "); // lfs\n";
    }
  } else if (ppc_inst.opcode() == 52) {
    
    uint32_t fS = ppc_inst.rs();
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    if (rA == 0) {
      out << "    ctx.mmu.write_f32(" << imm_s(simm) << ", (float)ctx.fpr[" << fS
          << "]); // stfs f" << fS << ", " << simm << "(0)\n";
    } else {
      out << "    ctx.mmu.write_f32(ctx.gpr[" << rA << "] + " << imm_s(simm)
          << ", (float)ctx.fpr[" << fS << "]); // stfs f" << fS << ", " << simm
          << "(r" << rA << ")\n";
    }
  } else if (ppc_inst.opcode() == 59) {
    // FP track: single-precision A-form ops write the rounded result to BOTH
    // paired-single lanes (Gekko; Dolphin ps[FD].Fill). frC of multiplies is
    // Force25Bit, fused madds round once to single, fres is the Gekko table
    // estimate (nWiiRuntime/include/runtime/gekko_fp.h).
    uint32_t xo = (ppc_inst.value() >> 1) & 0x1F;
    uint32_t fD = ppc_inst.rd();
    uint32_t fA = ppc_inst.ra();
    uint32_t fB = ppc_inst.rb();
    uint32_t fC = ppc_inst.rc();
    const std::string A = "ctx.fpr[" + std::to_string(fA) + "]";
    const std::string B = "ctx.fpr[" + std::to_string(fB) + "]";
    const std::string C = "ctx.fpr[" + std::to_string(fC) + "]";
    const std::string rs = "nwii::runtime::fp_round_single(";
    const std::string madd = "nwii::runtime::fp_madd_single(";
    const std::string neg = "nwii::runtime::fp_neg_unless_nan(";
    std::string expr, name;
    if (xo == 21) {
      expr = rs + A + " + " + B + ")"; name = "fadds";
    } else if (xo == 20) {
      expr = rs + A + " - " + B + ")"; name = "fsubs";
    } else if (xo == 25) {
      expr = rs + A + " * nwii::runtime::fp_force25(" + C + "))"; name = "fmuls";
    } else if (xo == 18) {
      expr = rs + A + " / " + B + ")"; name = "fdivs";
    } else if (xo == 22) {
      expr = rs + "std::sqrt(" + B + "))"; name = "fsqrts";
    } else if (xo == 24) {
      expr = "nwii::runtime::fp_fres(" + B + ")"; name = "fres";
    } else if (xo == 29) {
      expr = rs + madd + A + ", " + C + ", " + B + "))"; name = "fmadds";
    } else if (xo == 28) {
      expr = rs + madd + A + ", " + C + ", -" + B + "))"; name = "fmsubs";
    } else if (xo == 31) {
      expr = neg + rs + madd + A + ", " + C + ", " + B + ")))"; name = "fnmadds";
    } else if (xo == 30) {
      expr = neg + rs + madd + A + ", " + C + ", -" + B + ")))"; name = "fnmsubs";
    }
    if (!expr.empty()) {
      out << "    ctx.ps1[" << fD << "] = ctx.fpr[" << fD << "] = " << expr << "; // " << name << "\n";
    } else {
      out << "    std::cerr << \"UNIMPLEMENTED Opcode 59 XO \" << " << xo
          << " << \" at 0x\" << std::hex << ctx.pc << std::dec << \"\\n\"; "
             "std::exit(1);\n";
    }
  } else if (ppc_inst.opcode() == 63) {
    uint32_t xo = (ppc_inst.value() >> 1) & 0x3FF;
    uint32_t fD = ppc_inst.rd();
    uint32_t fB = ppc_inst.rb();
    uint32_t fA = ppc_inst.ra();
    if (xo == 72) {
      out << "    ctx.fpr[" << fD << "] = ctx.fpr[" << fB << "]; // fmr\n";
    } else if (xo == 40) { 
      out << "    ctx.fpr[" << fD << "] = -ctx.fpr[" << fB << "]; // fneg\n";
    } else if (xo == 264) { 
      out << "    ctx.fpr[" << fD << "] = std::fabs(ctx.fpr[" << fB
          << "]); // fabs\n";
    } else if (xo == 136) { 
      out << "    ctx.fpr[" << fD << "] = -std::fabs(ctx.fpr[" << fB
          << "]); // fnabs\n";
    } else if (xo == 12) { // frsp fills both lanes (Dolphin Fill)
      out << "    ctx.ps1[" << fD << "] = ctx.fpr[" << fD
          << "] = nwii::runtime::fp_round_single(ctx.fpr[" << fB
          << "]); // frsp\n";
    } else if (xo == 15 || xo == 14) { // fctiwz / fctiw (Dolphin ConvertToInteger)
      out << "    { const uint64_t bits = nwii::runtime::fp_fctiw_bits(ctx.fpr["
          << fB << "], " << (xo == 15 ? "true" : "false")
          << "); std::memcpy(&ctx.fpr[" << fD << "], &bits, 8); } // "
          << (xo == 15 ? "fctiwz" : "fctiw") << "\n";
    } else if (xo == 583) { // mffs: 0xFFF80000 | FPSCR
      out << "    { const uint64_t bits = 0xFFF8000000000000ull | ctx.fpscr; "
             "std::memcpy(&ctx.fpr[" << fD << "], &bits, 8); } // mffs\n";
    } else if (xo == 711) { // mtfsf FM, frB
      const uint32_t fm = (ppc_inst.value() >> 17) & 0xFF;
      uint32_t m = 0;
      for (uint32_t i = 0; i < 8; ++i)
        if (fm & (1u << i)) m |= 0xFu << (i * 4);
      out << "    { uint64_t bits; std::memcpy(&bits, &ctx.fpr[" << fB
          << "], 8); ctx.fpscr = (ctx.fpscr & ~0x" << std::hex << m
          << "u) | ((uint32_t)bits & 0x" << m << std::dec << "u); } // mtfsf\n";
    } else if (xo == 134) { // mtfsfi crfD, IMM
      const uint32_t shift = 4 * (7 - (fD >> 2));
      const uint32_t imm = (ppc_inst.value() >> 12) & 0xF;
      out << "    ctx.fpscr = (ctx.fpscr & ~0x" << std::hex << (0xFu << shift)
          << "u) | 0x" << (imm << shift) << std::dec << "u; // mtfsfi\n";
    } else if ((xo & 0x1F) == 26) { // frsqrte: Gekko table estimate, ps0 only
      out << "    ctx.fpr[" << fD << "] = nwii::runtime::fp_frsqrte(ctx.fpr[" << fB
          << "]); // frsqrte\n";
    } else if ((xo & 0x1F) == 24) { 
      out << "    ctx.fpr[" << fD << "] = 1.0 / ctx.fpr[" << fB
          << "]; // fre\n";
    } else if ((xo & 0x1F) == 18) { 
      out << "    ctx.fpr[" << fD << "] = ctx.fpr[" << fA << "] / ctx.fpr["
          << fB << "]; // fdiv\n";
    } else if ((xo & 0x1F) == 20) { 
      out << "    ctx.fpr[" << fD << "] = ctx.fpr[" << fA << "] - ctx.fpr["
          << fB << "]; // fsub\n";
    } else if ((xo & 0x1F) == 21) { 
      out << "    ctx.fpr[" << fD << "] = ctx.fpr[" << fA << "] + ctx.fpr["
          << fB << "]; // fadd\n";
    } else if ((xo & 0x1F) == 25) { 
      out << "    ctx.fpr[" << fD << "] = ctx.fpr[" << fA << "] * ctx.fpr["
          << ((ppc_inst.value() >> 6) & 0x1F) << "]; // fmul\n";
    } else if ((xo & 0x1F) == 29) { // fused (Dolphin NI_madd: std::fma)
      out << "    ctx.fpr[" << fD << "] = nwii::runtime::fp_fma(ctx.fpr[" << fA << "], ctx.fpr["
          << ((ppc_inst.value() >> 6) & 0x1F) << "], ctx.fpr[" << fB
          << "]); // fmadd\n";
    } else if ((xo & 0x1F) == 28) {
      out << "    ctx.fpr[" << fD << "] = nwii::runtime::fp_fma(ctx.fpr[" << fA << "], ctx.fpr["
          << ((ppc_inst.value() >> 6) & 0x1F) << "], -ctx.fpr[" << fB
          << "]); // fmsub\n";
    } else if ((xo & 0x1F) == 31) {
      out << "    ctx.fpr[" << fD << "] = nwii::runtime::fp_neg_unless_nan(nwii::runtime::fp_fma(ctx.fpr["
          << fA << "], ctx.fpr[" << ((ppc_inst.value() >> 6) & 0x1F) << "], ctx.fpr[" << fB
          << "])); // fnmadd\n";
    } else if ((xo & 0x1F) == 30) {
      out << "    ctx.fpr[" << fD << "] = nwii::runtime::fp_neg_unless_nan(nwii::runtime::fp_fma(ctx.fpr["
          << fA << "], ctx.fpr[" << ((ppc_inst.value() >> 6) & 0x1F) << "], -ctx.fpr[" << fB
          << "])); // fnmsub\n";
    } else if ((xo & 0x1F) == 23) { 
      out << "    ctx.fpr[" << fD << "] = (ctx.fpr[" << fA
          << "] >= 0.0) ? ctx.fpr[" << ((ppc_inst.value() >> 6) & 0x1F)
          << "] : ctx.fpr[" << fB << "]; // fsel\n";
    } else if (xo == 38) { // mtfsb1 crbD
      out << "    ctx.fpscr |= 0x" << std::hex << (0x80000000u >> fD) << std::dec
          << "u; // mtfsb1\n";
    } else if (xo == 70) { // mtfsb0 crbD
      out << "    ctx.fpscr &= ~0x" << std::hex << (0x80000000u >> fD) << std::dec
          << "u; // mtfsb0\n";
    } else if (xo == 64) { // mcrfs crfD, crfS (clears the exception bits it reads)
      const uint32_t shift = 4 * (7 - (fA >> 2));
      const uint32_t crf = fD >> 2;
      out << "    { const uint32_t f = (ctx.fpscr >> " << shift << ") & 0xF; ctx.fpscr &= ~0x"
          << std::hex << ((0xFu << shift) & 0x9FF80700u) << std::dec << "u; ctx.cr[" << crf
          << "].lt = (f & 8) != 0; ctx.cr[" << crf << "].gt = (f & 4) != 0; ctx.cr[" << crf
          << "].eq = (f & 2) != 0; ctx.cr[" << crf << "].so = (f & 1) != 0; } // mcrfs\n";
    } else if (xo == 0 || xo == 32) { 
      uint32_t crfD = fD >> 2;
      // FP track: IEEE relational operators are false for unordered operands, so
      // lt/gt/eq need no NaN test and FU = none of them (no std::isnan: MSVC
      // calls ucrtbase _dtest for it).
      out << "    { const double a = ctx.fpr[" << fA << "], b = ctx.fpr[" << fB
          << "]; const bool lt = a < b, gt = a > b, eq = a == b; ctx.cr[" << crfD
          << "].lt = lt; ctx.cr[" << crfD << "].gt = gt; ctx.cr[" << crfD << "].eq = eq; ctx.cr["
          << crfD << "].so = !(lt || gt || eq); } // fcmp\n";
    } else {
      out << "    std::cerr << \"UNIMPLEMENTED Opcode 63 XO \" << " << xo
          << " << \" at 0x\" << std::hex << ctx.pc << std::dec << \"\\n\"; "
             "std::exit(1);\n";
    }
  } else if (ppc_inst.opcode() == 20) {
    
    uint32_t rS = ppc_inst.rs();
    uint32_t rA = ppc_inst.ra();
    uint32_t sh = (ppc_inst.value() >> 11) & 0x1F;
    uint32_t mb = (ppc_inst.value() >> 6) & 0x1F;
    uint32_t me = (ppc_inst.value() >> 1) & 0x1F;

    uint32_t mask = 0;
    if (mb <= me) {
      for (int i = mb; i <= me; ++i)
        mask |= (1 << (31 - i));
    } else {
      for (int i = 0; i <= me; ++i)
        mask |= (1 << (31 - i));
      for (int i = mb; i <= 31; ++i)
        mask |= (1 << (31 - i));
    }

    out << "    ctx.gpr[" << rA << "] = (ctx.gpr[" << rA << "] & ~0x"
        << std::hex << std::uppercase << mask << std::dec
        << ") | (std::rotl(ctx.gpr[" << rS << "], " << sh << ") & 0x"
        << std::hex << std::uppercase << mask << std::dec << "); // rlwimi r"
        << rA << ", r" << rS << ", " << sh << ", " << mb << ", " << me << "\n";
    if (ppc_inst.value() & 1) {
      emit_cr0(out, rA);
    }
  } else if (ppc_inst.opcode() == 21) {
    
    uint32_t rS = ppc_inst.rs();
    uint32_t rA = ppc_inst.ra();
    uint32_t sh = (ppc_inst.value() >> 11) & 0x1F;
    uint32_t mb = (ppc_inst.value() >> 6) & 0x1F;
    uint32_t me = (ppc_inst.value() >> 1) & 0x1F;

    uint32_t mask = 0;
    if (mb <= me) {
      for (int i = mb; i <= me; ++i)
        mask |= (1 << (31 - i));
    } else {
      for (int i = 0; i <= me; ++i)
        mask |= (1 << (31 - i));
      for (int i = mb; i <= 31; ++i)
        mask |= (1 << (31 - i));
    }

    out << "    ctx.gpr[" << rA << "] = std::rotl(ctx.gpr[" << rS << "], " << sh
        << ") & 0x" << std::hex << std::uppercase << mask << std::dec
        << "; // rlwinm r" << rA << ", r" << rS << ", " << sh << ", " << mb
        << ", " << me << "\n";
    if (ppc_inst.value() & 1) {
      emit_cr0(out, rA);
    }
  } else if (ppc_inst.opcode() == 31) {
    uint32_t xo = ppc_inst.extended_opcode();
    uint32_t rD = ppc_inst.rd();
    uint32_t rA = ppc_inst.ra();
    uint32_t rB = ppc_inst.rb();
    uint32_t rS = ppc_inst.rs();

    if (xo == 598 || xo == 854 || xo == 982 || xo == 54 || xo == 86 ||
        xo == 278 || xo == 246 || xo == 470) {
      
      out << "    // sync/cache instruction\n";
    } else if (xo == 0) { 
      uint32_t crfD = rD >> 2;
      out << "    ctx.cr[" << crfD << "].lt = ((int32_t)ctx.gpr[" << rA
          << "] < (int32_t)ctx.gpr[" << rB << "]);\n";
      out << "    ctx.cr[" << crfD << "].gt = ((int32_t)ctx.gpr[" << rA
          << "] > (int32_t)ctx.gpr[" << rB << "]);\n";
      out << "    ctx.cr[" << crfD << "].eq = (ctx.gpr[" << rA
          << "] == ctx.gpr[" << rB << "]);\n";
      out << "    ctx.cr[" << crfD << "].so = (ctx.xer >> 31) & 1;\n";
    } else if (xo == 32) { 
      uint32_t crfD = rD >> 2;
      out << "    ctx.cr[" << crfD << "].lt = (ctx.gpr[" << rA << "] < ctx.gpr["
          << rB << "]);\n";
      out << "    ctx.cr[" << crfD << "].gt = (ctx.gpr[" << rA << "] > ctx.gpr["
          << rB << "]);\n";
      out << "    ctx.cr[" << crfD << "].eq = (ctx.gpr[" << rA
          << "] == ctx.gpr[" << rB << "]);\n";
      out << "    ctx.cr[" << crfD << "].so = (ctx.xer >> 31) & 1;\n";
    } else if (xo == 11) { 
      out << "    ctx.gpr[" << rD << "] = (uint32_t)(((uint64_t)ctx.gpr[" << rA
          << "] * (uint64_t)ctx.gpr[" << rB << "]) >> 32); // mulhwu\n";
      if (ppc_inst.value() & 1) { 
        emit_cr0(out, rD);
      }
    } else if (xo == 75) { 
      out << "    ctx.gpr[" << rD
          << "] = (uint32_t)(((int64_t)(int32_t)ctx.gpr[" << rA
          << "] * (int64_t)(int32_t)ctx.gpr[" << rB << "]) >> 32); // mulhw\n";
      if (ppc_inst.value() & 1) { 
        emit_cr0(out, rD);
      }
    } else if (xo == 87) {
      if (rA == 0)
        out << "    ctx.gpr[" << rD << "] = ctx.mmu.read8(ctx.gpr[" << rB
            << "]); // lbzx\n";
      else
        out << "    ctx.gpr[" << rD << "] = ctx.mmu.read8(ctx.gpr[" << rA
            << "] + ctx.gpr[" << rB << "]); // lbzx\n";
    } else if (xo == 215) {
      if (rA == 0)
        out << "    ctx.mmu.write8(ctx.gpr[" << rB << "], (uint8_t)ctx.gpr["
            << rS << "]); // stbx\n";
      else
        out << "    ctx.mmu.write8(ctx.gpr[" << rA << "] + ctx.gpr[" << rB
            << "], (uint8_t)ctx.gpr[" << rS << "]); // stbx\n";
    } else if (xo == 279) {
      if (rA == 0)
        out << "    ctx.gpr[" << rD << "] = ctx.mmu.read16(ctx.gpr[" << rB
            << "]); // lhzx\n";
      else
        out << "    ctx.gpr[" << rD << "] = ctx.mmu.read16(ctx.gpr[" << rA
            << "] + ctx.gpr[" << rB << "]); // lhzx\n";
    } else if (xo == 371) { 
      uint32_t spr_bottom = (ppc_inst.value() >> 16) & 0x1F;
      uint32_t spr_top    = (ppc_inst.value() >> 11) & 0x1F;
      uint32_t spr = (spr_top << 5) | spr_bottom;
      if (spr == 269) { 
        out << "    ctx.gpr[" << rD << "] = (uint32_t)(ctx.read_timebase() >> 32); // mftbu\n";
      } else { 
        out << "    ctx.gpr[" << rD << "] = (uint32_t)(ctx.read_timebase() & 0xFFFFFFFF); // mftb\n";
      }

      
      
    } else if (xo == 407) {
      if (rA == 0)
        out << "    ctx.mmu.write16(ctx.gpr[" << rB << "], (uint16_t)ctx.gpr["
            << rS << "]); // sthx\n";
      else
        out << "    ctx.mmu.write16(ctx.gpr[" << rA << "] + ctx.gpr[" << rB
            << "], (uint16_t)ctx.gpr[" << rS << "]); // sthx\n";
    } else if (xo == 23) {
      if (rA == 0)
        out << "    ctx.gpr[" << rD << "] = ctx.mmu.read32(ctx.gpr[" << rB
            << "]); // lwzx\n";
      else
        out << "    ctx.gpr[" << rD << "] = ctx.mmu.read32(ctx.gpr[" << rA
            << "] + ctx.gpr[" << rB << "]); // lwzx\n";
    } else if (xo == 151) {
      if (rA == 0)
        out << "    ctx.mmu.write32(ctx.gpr[" << rB << "], ctx.gpr[" << rS
            << "]); // stwx\n";
      else
        out << "    ctx.mmu.write32(ctx.gpr[" << rA << "] + ctx.gpr[" << rB
            << "], ctx.gpr[" << rS << "]); // stwx\n";
    } else if (xo == 40) {
      out << "    ctx.gpr[" << rD << "] = ctx.gpr[" << rB << "] - ctx.gpr["
          << rA << "]; // subf\n";
      if (ppc_inst.value() & 1) {
        emit_cr0(out, rD);
      }
    } else if (xo == 235) {
      out << "    ctx.gpr[" << rD << "] = ctx.gpr[" << rA << "] * ctx.gpr["
          << rB << "]; // mullw\n";
      if (ppc_inst.value() & 1) {
        emit_cr0(out, rD);
      }
    } else if (xo == 491 || xo == 1003) {
      // divw[o]: Dolphin Interpreter::divwx. A zero divisor or INT_MIN/-1 is
      // "undefined" on the 750CL; the hardware leaves 0 or -1 (by the sign of
      // rA), and the host must never execute the trapping division.
      out << "    {\n";
      out << "        const int32_t a = (int32_t)ctx.gpr[" << rA
          << "], b = (int32_t)ctx.gpr[" << rB << "];\n";
      out << "        const bool ovf = b == 0 || ((uint32_t)a == 0x80000000u && b == -1);\n";
      out << "        ctx.gpr[" << rD
          << "] = ovf ? (a < 0 ? 0xFFFFFFFFu : 0u) : (uint32_t)(a / b); // divw\n";
      if (xo == 1003)
        out << "        ctx.xer = ovf ? (ctx.xer | 0xC0000000u) : (ctx.xer & ~0x40000000u); // OV/SO\n";
      out << "    }\n";
      if (ppc_inst.value() & 1) {
        emit_cr0(out, rD);
      }
    } else if (xo == 459 || xo == 971) {
      // divwu[o]: Dolphin Interpreter::divwux, rD = 0 for a zero divisor.
      out << "    {\n";
      out << "        const uint32_t a = ctx.gpr[" << rA << "], b = ctx.gpr[" << rB
          << "];\n";
      out << "        ctx.gpr[" << rD << "] = b == 0 ? 0u : a / b; // divwu\n";
      if (xo == 971)
        out << "        ctx.xer = (b == 0) ? (ctx.xer | 0xC0000000u) : (ctx.xer & ~0x40000000u); // OV/SO\n";
      out << "    }\n";
      if (ppc_inst.value() & 1) {
        emit_cr0(out, rD);
      }
    } else if (xo == 954) {
      out << "    ctx.gpr[" << rA << "] = (uint32_t)(int32_t)(int8_t)ctx.gpr["
          << rS << "]; // extsb\n";
      if (ppc_inst.value() & 1) {
        emit_cr0(out, rA);
      }
    } else if (xo == 922) {
      out << "    ctx.gpr[" << rA << "] = (uint32_t)(int32_t)(int16_t)ctx.gpr["
          << rS << "]; // extsh\n";
      if (ppc_inst.value() & 1) {
        emit_cr0(out, rA);
      }
    } else if (xo == 28) {
      out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rS << "] & ctx.gpr["
          << rB << "]; // and\n";
      if (ppc_inst.value() & 1) {
        emit_cr0(out, rA);
      }
    } else if (xo == 60) {
      out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rS << "] & ~ctx.gpr["
          << rB << "]; // andc\n";
      if (ppc_inst.value() & 1) {
        emit_cr0(out, rA);
      }
    } else if (xo == 316) {
      out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rS << "] ^ ctx.gpr["
          << rB << "]; // xor\n";
      if (ppc_inst.value() & 1) {
        emit_cr0(out, rA);
      }
    } else if (xo == 24) {
      out << "    ctx.gpr[" << rA << "] = (ctx.gpr[" << rB
          << "] & 0x20) ? 0 : (ctx.gpr[" << rS << "] << (ctx.gpr[" << rB
          << "] & 0x1F)); // slw\n";
      if (ppc_inst.value() & 1) {
        emit_cr0(out, rA);
      }
    } else if (xo == 536) {
      out << "    ctx.gpr[" << rA << "] = (ctx.gpr[" << rB
          << "] & 0x20) ? 0 : (ctx.gpr[" << rS << "] >> (ctx.gpr[" << rB
          << "] & 0x1F)); // srw\n";
      if (ppc_inst.value() & 1) {
        emit_cr0(out, rA);
      }
    } else if (xo == 792) {
      // sraw: Dolphin Interpreter::srawx. CA is set when rS is negative and
      // any 1 bit is shifted out (all of them for a shift of 32..63).
      out << "    {\n";
      out << "        const int32_t sv = (int32_t)ctx.gpr[" << rS << "];\n";
      out << "        const uint32_t sh = ctx.gpr[" << rB << "] & 0x3F;\n";
      out << "        uint32_t ca;\n";
      out << "        if (sh & 0x20) { ctx.gpr[" << rA
          << "] = sv < 0 ? 0xFFFFFFFFu : 0u; ca = sv < 0 ? 1u : 0u; }\n";
      out << "        else { ctx.gpr[" << rA << "] = (uint32_t)(sv >> sh); "
             "ca = (sv < 0 && sh != 0 && ((uint32_t)sv << (32 - sh)) != 0) ? 1u : 0u; }\n";
      out << "        ctx.xer = (ctx.xer & ~0x20000000u) | (ca << 29); // CA\n";
      out << "    } // sraw\n";
      if (ppc_inst.value() & 1) {
        emit_cr0(out, rA);
      }
    } else if (xo == 26) {
      out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rS
          << "] == 0 ? 32 : std::countl_zero(ctx.gpr[" << rS
          << "]); // cntlzw\n";
      if (ppc_inst.value() & 1) { 
        emit_cr0(out, rA);
      }
    } else if (xo == 10) { 
      out << "    {\n";
      out << "        uint64_t res = (uint64_t)ctx.gpr[" << rA
          << "] + (uint64_t)ctx.gpr[" << rB << "];\n";
      out << "        ctx.gpr[" << rD << "] = (uint32_t)res;\n";
      out << "        ctx.xer = (ctx.xer & ~(1 << 29)) | (((res >> 32) & 1) << "
             "29); // Set CA bit\n";
      if (ppc_inst.value() & 1) {
        emit_cr0(out, rD, "        ");
      }
      out << "    }\n";
    } else if (xo == 8) { 
      out << "    {\n";
      out << "        uint64_t res = (uint64_t)(~ctx.gpr[" << rA
          << "]) + (uint64_t)ctx.gpr[" << rB << "] + 1;\n";
      out << "        ctx.gpr[" << rD << "] = (uint32_t)res;\n";
      out << "        ctx.xer = (ctx.xer & ~(1 << 29)) | (((res >> 32) & 1) << "
             "29); // Set CA bit\n";
      if (ppc_inst.value() & 1) {
        emit_cr0(out, rD, "        ");
      }
      out << "    }\n";
    } else if (xo == 104 || xo == 616) { 
      out << "    ctx.gpr[" << rD << "] = (uint32_t)(-(int32_t)ctx.gpr[" << rA
          << "]); // neg\n";
      if (ppc_inst.value() & 1) {
        emit_cr0(out, rD);
      }
    } else if (xo == 266) { 
      out << "    ctx.gpr[" << rD << "] = ctx.gpr[" << rA << "] + ctx.gpr["
          << rB << "];\n";
      if (ppc_inst.value() & 1) {
        emit_cr0(out, rD);
      }
    } else if (xo == 138) { 
      out << "    {\n";
      out << "        uint64_t res = (uint64_t)ctx.gpr[" << rA
          << "] + (uint64_t)ctx.gpr[" << rB
          << "] + (uint64_t)((ctx.xer >> 29) & 1);\n";
      out << "        ctx.gpr[" << rD << "] = (uint32_t)res;\n";
      out << "        ctx.xer = (ctx.xer & ~(1 << 29)) | (((res >> 32) & 1) << "
             "29); // Set CA bit\n";
      if (ppc_inst.value() & 1) {
        emit_cr0(out, rD, "        ");
      }
      out << "    }\n";
    } else if (xo == 136) { 
      out << "    {\n";
      out << "        uint64_t res = (uint64_t)(~ctx.gpr[" << rA
          << "]) + (uint64_t)ctx.gpr[" << rB
          << "] + (uint64_t)((ctx.xer >> 29) & 1);\n";
      out << "        ctx.gpr[" << rD << "] = (uint32_t)res;\n";
      out << "        ctx.xer = (ctx.xer & ~(1 << 29)) | (((res >> 32) & 1) << "
             "29); // Set CA bit\n";
      if (ppc_inst.value() & 1) {
        emit_cr0(out, rD, "        ");
      }
      out << "    }\n";
    } else if (xo == 202) { 
      out << "    {\n";
      out << "        uint64_t res = (uint64_t)ctx.gpr[" << rA
          << "] + (uint64_t)((ctx.xer >> 29) & 1);\n";
      out << "        ctx.gpr[" << rD << "] = (uint32_t)res;\n";
      out << "        ctx.xer = (ctx.xer & ~(1 << 29)) | (((res >> 32) & 1) << "
             "29); // Set CA bit\n";
      if (ppc_inst.value() & 1) {
        emit_cr0(out, rD, "        ");
      }
      out << "    }\n";
    } else if (xo == 200) { 
      out << "    {\n";
      out << "        uint64_t res = (uint64_t)(~ctx.gpr[" << rA
          << "]) + (uint64_t)((ctx.xer >> 29) & 1);\n";
      out << "        ctx.gpr[" << rD << "] = (uint32_t)res;\n";
      out << "        ctx.xer = (ctx.xer & ~(1 << 29)) | (((res >> 32) & 1) << "
             "29); // Set CA bit\n";
      if (ppc_inst.value() & 1) {
        emit_cr0(out, rD, "        ");
      }
      out << "    }\n";
    } else if (xo == 234) { 
      out << "    {\n";
      out << "        uint64_t res = (uint64_t)ctx.gpr[" << rA
          << "] + 0xFFFFFFFFULL + (uint64_t)((ctx.xer >> 29) & 1);\n";
      out << "        ctx.gpr[" << rD << "] = (uint32_t)res;\n";
      out << "        ctx.xer = (ctx.xer & ~(1 << 29)) | (((res >> 32) & 1) << "
             "29); // Set CA bit\n";
      if (ppc_inst.value() & 1) {
        emit_cr0(out, rD, "        ");
      }
      out << "    }\n";
    } else if (xo == 232) { 
      out << "    {\n";
      out << "        uint64_t res = (uint64_t)(~ctx.gpr[" << rA
          << "]) + 0xFFFFFFFFULL + (uint64_t)((ctx.xer >> 29) & 1);\n";
      out << "        ctx.gpr[" << rD << "] = (uint32_t)res;\n";
      out << "        ctx.xer = (ctx.xer & ~(1 << 29)) | (((res >> 32) & 1) << "
             "29); // Set CA bit\n";
      if (ppc_inst.value() & 1) {
        emit_cr0(out, rD, "        ");
      }
      out << "    }\n";
    } else if (xo == 83) { 
      out << "    ctx.gpr[" << ppc_inst.rd() << "] = ctx.msr;\n";
    } else if (xo == 146) { 

      

      
      out << "    {\n";
      out << "        uint32_t ee_was = ctx.msr & 0x8000;\n";
      out << "        ctx.msr = ctx.gpr[" << ppc_inst.rs() << "];\n";
      // M1.11: FP Unavailable when MSR[FP]=0 (e.g. OSSetCurrentContext
      // switching to a context that does not own the FPU).
      out << "        ctx.fp_unavailable_check();\n";
      out << "        if (!ee_was && (ctx.msr & 0x8000)) {\n";
      out << "            ctx.pc = " << pcl(inst.address + 4) << ";\n";
      out << "            if (process_pending_callbacks(ctx)) return;\n";
      out << "        }\n";
      out << "    }\n";
    } else if (xo == 124) { 
      out << "    ctx.gpr[" << rA << "] = ~(ctx.gpr[" << rS << "] | ctx.gpr["
          << rB << "]); // nor\n";
      if (ppc_inst.value() & 1) {
        emit_cr0(out, rA);
      }
    } else if (xo == 476) { 
      out << "    ctx.gpr[" << rA << "] = ~(ctx.gpr[" << rS << "] & ctx.gpr["
          << rB << "]); // nand\n";
      if (ppc_inst.value() & 1) {
        emit_cr0(out, rA);
      }
    } else if (xo == 284) { 
      out << "    ctx.gpr[" << rA << "] = ~(ctx.gpr[" << rS << "] ^ ctx.gpr["
          << rB << "]); // eqv\n";
      if (ppc_inst.value() & 1) {
        emit_cr0(out, rA);
      }
    } else if (xo == 412) { 
      out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rS << "] | ~ctx.gpr["
          << rB << "]; // orc\n";
      if (ppc_inst.value() & 1) {
        emit_cr0(out, rA);
      }
    } else if (xo == 20) {
      out << "    ctx.gpr[" << rD << "] = ctx.mmu.read32(" << ea_base(rA)
          << " + ctx.gpr[" << rB << "]); // lwarx\n";
    } else if (xo == 150) {
      out << "    ctx.mmu.write32(" << ea_base(rA) << " + ctx.gpr[" << rB
          << "], ctx.gpr[" << ppc_inst.rs() << "]); // stwcx\n";
      out << "    ctx.cr[0].eq = true; ctx.cr[0].lt = false; ctx.cr[0].gt = "
             "false; ctx.cr[0].so = (ctx.xer >> 31) & 1; // always succeed\n";
    } else if (xo == 119) { 
      out << "    { uint32_t ea = ctx.gpr[" << rA << "] + ctx.gpr[" << rB
          << "]; ";
      out << "ctx.gpr[" << rD << "] = ctx.mmu.read8(ea); ";
      out << "ctx.gpr[" << rA << "] = ea; } // lbzux\n";
    } else if (xo == 311) { 
      out << "    { uint32_t ea = ctx.gpr[" << rA << "] + ctx.gpr[" << rB
          << "]; ";
      out << "ctx.gpr[" << rD << "] = ctx.mmu.read16(ea); ";
      out << "ctx.gpr[" << rA << "] = ea; } // lhzux\n";
    } else if (xo == 343) { 
      out << "    ctx.gpr[" << rD
          << "] = (uint32_t)(int32_t)(int16_t)ctx.mmu.read16(" << ea_base(rA)
          << " + ctx.gpr[" << rB << "]); // lhax\n";
    } else if (xo == 375) { 
      out << "    { uint32_t ea = ctx.gpr[" << rA << "] + ctx.gpr[" << rB
          << "]; ";
      out << "ctx.gpr[" << rD
          << "] = (uint32_t)(int32_t)(int16_t)ctx.mmu.read16(ea); ";
      out << "ctx.gpr[" << rA << "] = ea; } // lhaux\n";
    } else if (xo == 247) { 
      out << "    {\n";
      out << "        uint32_t ea = ctx.gpr[" << rA << "] + ctx.gpr[" << rB
          << "];\n";
      out << "        ctx.mmu.write8(ea, (uint8_t)ctx.gpr[" << ppc_inst.rs()
          << "]);\n";
      out << "        ctx.gpr[" << rA << "] = ea;\n";
      out << "    } // stbux\n";
    } else if (xo == 439) { 
      out << "    {\n";
      out << "        uint32_t ea = ctx.gpr[" << rA << "] + ctx.gpr[" << rB
          << "];\n";
      out << "        ctx.mmu.write16(ea, (uint16_t)ctx.gpr[" << ppc_inst.rs()
          << "]);\n";
      out << "        ctx.gpr[" << rA << "] = ea;\n";
      out << "    } // sthux\n";
    } else if (xo == 824) {

      

      uint32_t sh = (ppc_inst.value() >> 11) & 0x1F;
      out << "    {\n";
      out << "        int32_t sv = (int32_t)ctx.gpr[" << rD << "];\n";
      out << "        ctx.gpr[" << rA << "] = (uint32_t)(sv >> " << sh << ");\n";
      out << "        uint32_t ca = (sv < 0 && (uint32_t)(sv & "
          << (sh == 0 ? 0u : ((1u << sh) - 1)) << "u) != 0) ? 1 : 0;\n";
      out << "        ctx.xer = (ctx.xer & ~(1u << 29)) | (ca << 29);\n";
      if (ppc_inst.value() & 1)
        emit_cr0(out, rA, "        ");
      out << "    } // srawi\n";
    } else if (xo == 535) { 
      uint32_t frD2 = ppc_inst.rd();
      out << "    ctx.fpr[" << frD2 << "] = ctx.ps1[" << frD2
          << "] = (double)ctx.mmu.read_f32(" << ea_base(rA) << " + ctx.gpr["
          << rB << "]); // lfsx (both PS lanes)\n";
    } else if (xo == 567) { 
      uint32_t frD2 = ppc_inst.rd();
      out << "    { uint32_t ea = ctx.gpr[" << rA << "] + ctx.gpr[" << rB
          << "]; ";
      out << "ctx.fpr[" << frD2 << "] = ctx.ps1[" << frD2
          << "] = (double)ctx.mmu.read_f32(ea); ";
      out << "ctx.gpr[" << rA << "] = ea; } // lfsux (both PS lanes)\n";
    } else if (xo == 599) { 
      uint32_t frD2 = ppc_inst.rd();
      out << "    ctx.fpr[" << frD2 << "] = ctx.mmu.read_f64(" << ea_base(rA)
          << " + ctx.gpr[" << rB << "]); // lfdx\n";
    } else if (xo == 631) { 
      uint32_t frD2 = ppc_inst.rd();
      out << "    { uint32_t ea = ctx.gpr[" << rA << "] + ctx.gpr[" << rB
          << "]; ";
      out << "ctx.fpr[" << frD2 << "] = ctx.mmu.read_f64(ea); ";
      out << "ctx.gpr[" << rA << "] = ea; } // lfdux\n";
    } else if (xo == 663) { 
      out << "    ctx.mmu.write_f32(" << ea_base(rA) << " + ctx.gpr[" << rB
          << "], (float)ctx.fpr[" << ppc_inst.rs() << "]); // stfsx\n";
    } else if (xo == 695) { 
      out << "    ctx.mmu.write_f32(ctx.gpr[" << rA << "] + ctx.gpr[" << rB
          << "], (float)ctx.fpr[" << ppc_inst.rs() << "]); // stfsux\n";
      out << "    ctx.gpr[" << rA << "] += ctx.gpr[" << rB << "];\n";
    } else if (xo == 727) { 
      out << "    ctx.mmu.write_f64(" << ea_base(rA) << " + ctx.gpr[" << rB
          << "], ctx.fpr[" << ppc_inst.rs() << "]); // stfdx\n";
    } else if (xo == 759) { 
      out << "    ctx.mmu.write_f64(ctx.gpr[" << rA << "] + ctx.gpr[" << rB
          << "], ctx.fpr[" << ppc_inst.rs() << "]); // stfdux\n";
      out << "    ctx.gpr[" << rA << "] += ctx.gpr[" << rB << "];\n";
    } else if (xo == 983) { 
      // stfiwx stores the low word of the raw FPR bits (the fctiw[z] result
      // is kept as bits, see fctiwz above), as in Dolphin Interpreter::stfiwx.
      out << "    { uint64_t bits; std::memcpy(&bits, &ctx.fpr[" << ppc_inst.rs()
          << "], 8); ctx.mmu.write32(" << ea_base(rA) << " + ctx.gpr[" << rB
          << "], (uint32_t)bits); } // stfiwx\n";
    } else if (xo == 790) {
      out << "    { uint16_t v = ctx.mmu.read16(" << ea_base(rA)
          << " + ctx.gpr[" << rB << "]); ctx.gpr[" << rD
          << "] = ((v&0xFF)<<8)|((v>>8)&0xFF); } // lhbrx\n";
    } else if (xo == 534) {
      out << "    { uint32_t v = ctx.mmu.read32(" << ea_base(rA)
          << " + ctx.gpr[" << rB << "]); ctx.gpr[" << rD
          << "] = __builtin_bswap32(v); } // lwbrx\n";
    } else if (xo == 662) {
      out << "    ctx.mmu.write32(" << ea_base(rA) << " + ctx.gpr[" << rB
          << "], __builtin_bswap32(ctx.gpr[" << ppc_inst.rs()
          << "])); // stwbrx\n";
    } else if (xo == 1014) {
      out << "    ctx.zero_cache_line(";
      if (rA != 0) out << "ctx.gpr[" << rA << "] + ";
      out << "ctx.gpr[" << rB << "]); // dcbz\n";
    } else if (xo == 55) { 
      out << "    { uint32_t ea = ctx.gpr[" << rA << "] + ctx.gpr[" << rB
          << "]; ";
      out << "ctx.gpr[" << rD << "] = ctx.mmu.read32(ea); ";
      out << "ctx.gpr[" << rA << "] = ea; } // lwzux\n";
    } else if (xo == 183) { 
      out << "    {\n";
      out << "        uint32_t ea = ctx.gpr[" << rA << "] + ctx.gpr[" << rB
          << "];\n";
      out << "        ctx.mmu.write32(ea, ctx.gpr[" << ppc_inst.rs() << "]);\n";
      out << "        ctx.gpr[" << rA << "] = ea;\n";
      out << "    } // stwux\n";
    } else if (xo == 918) { 
      out << "    ctx.mmu.write16(" << ea_base(rA) << " + ctx.gpr[" << rB
          << "], ((ctx.gpr[" << ppc_inst.rs() << "] & 0xFF) << 8) | ((ctx.gpr["
          << ppc_inst.rs() << "] >> 8) & 0xFF)); // sthbrx\n";
    } else if (xo == 595 || xo == 210) { 
      out << "    // mfsr/mtsr stub\n";
    } else if (xo == 19) { 
      out << "    {\n";
      out << "        uint32_t cr_val = 0;\n";
      out << "        for (int i = 0; i < 8; ++i) {\n";
      out << "            cr_val |= (ctx.cr[i].lt ? 8 : 0) << (28 - i * 4);\n";
      out << "            cr_val |= (ctx.cr[i].gt ? 4 : 0) << (28 - i * 4);\n";
      out << "            cr_val |= (ctx.cr[i].eq ? 2 : 0) << (28 - i * 4);\n";
      out << "            cr_val |= (ctx.cr[i].so ? 1 : 0) << (28 - i * 4);\n";
      out << "        }\n";
      out << "        ctx.gpr[" << rD << "] = cr_val;\n";
      out << "    }\n";
    } else if (xo == 144) { 
      uint32_t crm = (ppc_inst.value() >> 12) & 0xFF;
      out << "    {\n";
      out << "        uint32_t cr_val = ctx.gpr[" << ppc_inst.rs() << "];\n";
      out << "        uint32_t mask = 0x" << std::hex << crm << std::dec
          << ";\n";
      out << "        for (int i = 0; i < 8; ++i) {\n";
      out << "            if (mask & (1 << (7 - i))) {\n";
      out << "                uint32_t field = (cr_val >> (28 - i * 4)) & "
             "0xF;\n";
      out << "                ctx.cr[i].lt = (field & 8) != 0;\n";
      out << "                ctx.cr[i].gt = (field & 4) != 0;\n";
      out << "                ctx.cr[i].eq = (field & 2) != 0;\n";
      out << "                ctx.cr[i].so = (field & 1) != 0;\n";
      out << "            }\n";
      out << "        }\n";
      out << "    }\n";
    } else if (xo == 566) { 
      out << "    // tlbsync (no-op)\n";
    } else {
      out << "    std::cerr << \"UNIMPLEMENTED Opcode 31 XO \" << " << xo
          << " << \" at 0x\" << std::hex << ctx.pc << std::dec << \"\\n\"; "
             "std::exit(1);\n";
    }
  } else if (ppc_inst.opcode() == 24) { 
    out << "    ctx.gpr[" << ppc_inst.ra() << "] = ctx.gpr[" << ppc_inst.rs()
        << "] | " << ppc_inst.uimm() << "; // ori\n";
  } else if (ppc_inst.opcode() == 25) { 
    out << "    ctx.gpr[" << ppc_inst.ra() << "] = ctx.gpr[" << ppc_inst.rs()
        << "] | " << (ppc_inst.uimm() << 16) << "; // oris\n";
  } else if (ppc_inst.opcode() == 26) { 
    out << "    ctx.gpr[" << ppc_inst.ra() << "] = ctx.gpr[" << ppc_inst.rs()
        << "] ^ " << ppc_inst.uimm() << "; // xori\n";
  } else if (ppc_inst.opcode() == 27) { 
    out << "    ctx.gpr[" << ppc_inst.ra() << "] = ctx.gpr[" << ppc_inst.rs()
        << "] ^ " << (ppc_inst.uimm() << 16) << "; // xoris\n";
  } else if (ppc_inst.opcode() == 28) { 
    uint32_t rA = ppc_inst.ra();
    out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << ppc_inst.rs() << "] & "
        << ppc_inst.uimm() << "; // andi.\n";
    emit_cr0(out, rA);
  } else if (ppc_inst.opcode() == 29) {
    // andis. is a record form like andi.: it always writes CR0.
    const uint32_t rA = ppc_inst.ra();
    out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << ppc_inst.rs() << "] & 0x"
        << std::hex << std::uppercase << ((uint32_t)ppc_inst.uimm() << 16)
        << std::dec << "u; // andis.\n";
    emit_cr0(out, rA);
  } else if (ppc_inst.opcode() == 7) {
    out << "    ctx.gpr[" << ppc_inst.rd() << "] = (uint32_t)((int32_t)ctx.gpr["
        << ppc_inst.ra() << "] * " << ppc_inst.simm() << "); // mulli\n";
  } else if (ppc_inst.opcode() == 8) {
    // subfic: rD = ~rA + EXTS(SIMM) + 1, CA = carry out of bit 0 (Dolphin
    // Interpreter::subfic). __div2i/__mod2i/__cvt_sll_dbl chain CA from here
    // into subfze/subfe for 64-bit negation.
    const uint32_t imm = (uint32_t)(int32_t)ppc_inst.simm();
    out << "    {\n";
    out << "        const uint64_t res = (uint64_t)(uint32_t)~ctx.gpr["
        << ppc_inst.ra() << "] + 0x" << std::hex << std::uppercase << imm
        << std::dec << "ull + 1;\n";
    out << "        ctx.gpr[" << ppc_inst.rd() << "] = (uint32_t)res; // subfic\n";
    emit_set_ca_from_res(out);
    out << "    }\n";
  } else if (ppc_inst.opcode() == 12) {
    uint32_t rD = ppc_inst.rd();
    uint32_t rA = ppc_inst.ra();
    // CA is the carry out of the 32-bit add of rA and EXTS(SIMM): the
    // immediate must enter the 64-bit sum zero-extended from 32 bits, or
    // every negative immediate loses its carry (addic rX,rX,-1 on rX != 0).
    const uint32_t imm = (uint32_t)(int32_t)ppc_inst.simm();
    out << "    {\n";
    out << "        uint64_t res = (uint64_t)ctx.gpr[" << rA << "] + "
        << imm_x(imm) << ";\n";
    out << "        ctx.gpr[" << rD << "] = (uint32_t)res;\n";
    out << "        ctx.xer = (ctx.xer & ~(1 << 29)) | (((res >> 32) & 1) << "
           "29); // CA\n";
    out << "    }\n";
  } else if (ppc_inst.opcode() == 13) { 
    uint32_t rD = ppc_inst.rd();
    uint32_t rA = ppc_inst.ra();
    const uint32_t imm = (uint32_t)(int32_t)ppc_inst.simm(); // see addic
    out << "    {\n";
    out << "        uint64_t res = (uint64_t)ctx.gpr[" << rA << "] + "
        << imm_x(imm) << ";\n";
    out << "        ctx.gpr[" << rD << "] = (uint32_t)res;\n";
    out << "        ctx.xer = (ctx.xer & ~(1 << 29)) | (((res >> 32) & 1) << "
           "29); // CA\n";
    emit_cr0(out, rD, "        ");
    out << "    }\n";
  } else if (ppc_inst.opcode() == 15) { 
    uint32_t rA = ppc_inst.ra();
    if (rA == 0)
      out << "    ctx.gpr[" << ppc_inst.rd()
          << "] = " << imm_hi(ppc_inst.simm()) << "; // lis\n";
    else
      out << "    ctx.gpr[" << ppc_inst.rd() << "] = ctx.gpr[" << rA << "] + "
          << imm_hi(ppc_inst.simm()) << "; // addis\n";
  } else if (ppc_inst.opcode() == 17) { 
    out << "    // sc (System Call)\n";
    // The handler can save this context and yield to an interrupt/callback.
    // SC is complete at that point, so resume at its following instruction.
    out << "    ctx.pc = " << pcl(inst.address + 4) << ";\n";
    out << "    if (nwii::runtime::handle_syscall(ctx)) return;\n";
  } else if (ppc_inst.opcode() == 4) { 
    uint32_t xo = (inst.opcode >> 1) & 0x1F;
    uint32_t xo_6 = (inst.opcode >> 1) & 0x3F;
    uint32_t xo_10 = (inst.opcode >> 1) & 0x3FF;
    uint32_t frD = ppc_inst.rd();
    uint32_t frA = ppc_inst.ra();
    uint32_t frB = ppc_inst.rb();
    uint32_t frC = ppc_inst.rc();

    if (xo_6 == 6 || xo_6 == 7 || xo_6 == 38 || xo_6 == 39) {
      const bool update = xo_6 == 38 || xo_6 == 39;
      const bool load = xo_6 == 6 || xo_6 == 38;
      const uint32_t w = (inst.opcode >> 10) & 1;
      const uint32_t i = (inst.opcode >> 7) & 7;
      std::stringstream address;
      if (update || frA != 0)
        address << "ctx.gpr[" << frA << "] + ";
      address << "ctx.gpr[" << frB << "]";
      if (update) {
        out << "    { const uint32_t ea = " << address.str() << ";\n";
        out << "      ctx.psq_" << (load ? "load" : "store") << "(" << frD
            << ", ea, " << w << ", " << i << ");\n";
        out << "      ctx.gpr[" << frA << "] = ea; }\n";
      } else {
        out << "    ctx.psq_" << (load ? "load" : "store") << "(" << frD
            << ", " << address.str() << ", " << w << ", " << i << ");\n";
      }
    } else if (xo == 21) {
      out << "    ctx.ps_add(" << frD << ", " << frA << ", " << frB << ");\n";
    } else if (xo == 20) {
      out << "    ctx.ps_sub(" << frD << ", " << frA << ", " << frB << ");\n";
    } else if (xo == 25) {
      out << "    ctx.ps_mul(" << frD << ", " << frA << ", " << frC << ");\n";
    } else if (xo == 29) {
      out << "    ctx.ps_madd(" << frD << ", " << frA << ", " << frC << ", "
          << frB << ");\n";
    } else if (xo == 28) {
      out << "    ctx.ps_msub(" << frD << ", " << frA << ", " << frC << ", "
          << frB << ");\n";
    } else if (xo == 31) {
      out << "    ctx.ps_nmadd(" << frD << ", " << frA << ", " << frC << ", "
          << frB << ");\n";
    } else if (xo == 30) {
      out << "    ctx.ps_nmsub(" << frD << ", " << frA << ", " << frC << ", "
          << frB << ");\n";
    } else if (xo == 18) {
      out << "    ctx.ps_div(" << frD << ", " << frA << ", " << frB << ");\n";
    } else if (xo == 10) {
      out << "    ctx.ps_sum0(" << frD << ", " << frA << ", " << frC << ", "
          << frB << ");\n";
    } else if (xo == 11) {
      out << "    ctx.ps_sum1(" << frD << ", " << frA << ", " << frC << ", "
          << frB << ");\n";
    } else if (xo == 12) {
      out << "    ctx.ps_muls0(" << frD << ", " << frA << ", " << frC << ");\n";
    } else if (xo == 13) {
      out << "    ctx.ps_muls1(" << frD << ", " << frA << ", " << frC << ");\n";
    } else if (xo == 14) {
      out << "    ctx.ps_madds0(" << frD << ", " << frA << ", " << frC << ", "
          << frB << ");\n";
    } else if (xo == 15) {
      out << "    ctx.ps_madds1(" << frD << ", " << frA << ", " << frC << ", "
          << frB << ");\n";
    } else if (xo == 23) {
      out << "    ctx.ps_sel(" << frD << ", " << frA << ", " << frC << ", "
          << frB << ");\n";
    } else if (xo == 24) {
      out << "    ctx.ps_res(" << frD << ", " << frB << ");\n";
    } else if (xo == 26) {
      out << "    ctx.ps_rsqrte(" << frD << ", " << frB << ");\n";
    } else if (xo_10 == 72) {
      out << "    ctx.ps_mr(" << frD << ", " << frB << ");\n";
    } else if (xo_10 == 40) {
      out << "    ctx.ps_neg(" << frD << ", " << frB << ");\n";
    } else if (xo_10 == 264) {
      out << "    ctx.ps_abs(" << frD << ", " << frB << ");\n";
    } else if (xo_10 == 136) {
      out << "    ctx.ps_nabs(" << frD << ", " << frB << ");\n";
    } else if (xo_10 == 528) {
      out << "    ctx.ps_merge00(" << frD << ", " << frA << ", " << frB
          << ");\n";
    } else if (xo_10 == 560) {
      out << "    ctx.ps_merge01(" << frD << ", " << frA << ", " << frB
          << ");\n";
    } else if (xo_10 == 592) {
      out << "    ctx.ps_merge10(" << frD << ", " << frA << ", " << frB
          << ");\n";
    } else if (xo_10 == 624) {
      out << "    ctx.ps_merge11(" << frD << ", " << frA << ", " << frB
          << ");\n";
    } else if (xo_10 == 0) {
      out << "    ctx.ps_cmpu0(" << (frD >> 2) << ", " << frA << ", " << frB
          << ");\n";
    } else if (xo_10 == 32) {
      out << "    ctx.ps_cmpo0(" << (frD >> 2) << ", " << frA << ", " << frB
          << ");\n";
    } else if (xo_10 == 64) {
      out << "    ctx.ps_cmpu1(" << (frD >> 2) << ", " << frA << ", " << frB
          << ");\n";
    } else if (xo_10 == 96) {
      out << "    ctx.ps_cmpo1(" << (frD >> 2) << ", " << frA << ", " << frB
          << ");\n";
    } else if (xo_10 == 1014) {
      out << "    ctx.zero_cache_line(";
      if (frA != 0) out << "ctx.gpr[" << frA << "] + ";
      out << "ctx.gpr[" << frB << "]); // dcbz_l\n";
    } else {
      out << "    std::cerr << \"UNIMPLEMENTED Opcode 4 (ps_*) XO \" << " << xo
          << " << \" XO_10 \" << " << xo_10
          << " << \" at 0x\" << std::hex << ctx.pc << std::dec << \"\\n\"; "
             "std::exit(1);\n";
      out << "    ctx.fpr[" << frD << "] = 0.0;\n";
      out << "    ctx.ps1[" << frD << "] = 0.0;\n";
    }
  } else if (ppc_inst.opcode() == 10) { 
    uint32_t crD = ppc_inst.rd() >> 2;
    out << "    ctx.cr[" << crD << "].lt = (ctx.gpr[" << ppc_inst.ra() << "] < "
        << ppc_inst.uimm() << ");\n";
    out << "    ctx.cr[" << crD << "].gt = (ctx.gpr[" << ppc_inst.ra() << "] > "
        << ppc_inst.uimm() << ");\n";
    out << "    ctx.cr[" << crD << "].eq = (ctx.gpr[" << ppc_inst.ra()
        << "] == " << ppc_inst.uimm() << ");\n";
    out << "    ctx.cr[" << crD << "].so = (ctx.xer >> 31) & 1;\n";
  } else if (ppc_inst.opcode() == 11) {
    uint32_t crD = ppc_inst.rd() >> 2;
    out << "    ctx.cr[" << crD << "].lt = ((int32_t)ctx.gpr[" << ppc_inst.ra()
        << "] < " << ppc_inst.simm() << ");\n";
    out << "    ctx.cr[" << crD << "].gt = ((int32_t)ctx.gpr[" << ppc_inst.ra()
        << "] > " << ppc_inst.simm() << ");\n";
    out << "    ctx.cr[" << crD << "].eq = ((int32_t)ctx.gpr[" << ppc_inst.ra()
        << "] == " << ppc_inst.simm() << ");\n";
    out << "    ctx.cr[" << crD << "].so = (ctx.xer >> 31) & 1;\n";
  } else if (ppc_inst.opcode() == 34) { 
    uint32_t rA = ppc_inst.ra();
    if (rA == 0)
      out << "    ctx.gpr[" << ppc_inst.rd() << "] = ctx.mmu.read8("
          << ppc_inst.simm() << "); // lbz\n";
    else
      out << "    ctx.gpr[" << ppc_inst.rd() << "] = ctx.mmu.read8(ctx.gpr["
          << rA << "] + " << ppc_inst.simm() << "); // lbz\n";
  } else if (ppc_inst.opcode() == 38) { 
    uint32_t rA = ppc_inst.ra();
    if (rA == 0)
      out << "    ctx.mmu.write8(" << ppc_inst.simm() << ", (uint8_t)ctx.gpr["
          << ppc_inst.rs() << "]); // stb\n";
    else
      out << "    ctx.mmu.write8(ctx.gpr[" << rA << "] + " << ppc_inst.simm()
          << ", (uint8_t)ctx.gpr[" << ppc_inst.rs() << "]); // stb\n";
  } else if (ppc_inst.opcode() == 40) { 
    uint32_t rA = ppc_inst.ra();
    if (rA == 0)
      out << "    ctx.gpr[" << ppc_inst.rd() << "] = ctx.mmu.read16("
          << imm_s(ppc_inst.simm()) << "); // lhz\n";
    else
      out << "    ctx.gpr[" << ppc_inst.rd() << "] = ctx.mmu.read16(ctx.gpr["
          << rA << "] + " << imm_s(ppc_inst.simm()) << "); // lhz\n";
  } else if (ppc_inst.opcode() == 41) { 
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rA << "] + " << imm_s(simm)
        << ";\n";
    out << "    ctx.gpr[" << ppc_inst.rd() << "] = ctx.mmu.read16(ctx.gpr["
        << rA << "]); // lhzu\n";
  } else if (ppc_inst.opcode() == 42) { 
    uint32_t rA = ppc_inst.ra();
    if (rA == 0)
      out << "    ctx.gpr[" << ppc_inst.rd()
          << "] = (uint32_t)(int32_t)(int16_t)ctx.mmu.read16("
          << imm_s(ppc_inst.simm()) << "); // lha\n";
    else
      out << "    ctx.gpr[" << ppc_inst.rd()
          << "] = (uint32_t)(int32_t)(int16_t)ctx.mmu.read16(ctx.gpr[" << rA
          << "] + " << imm_s(ppc_inst.simm()) << "); // lha\n";
  } else if (ppc_inst.opcode() == 43) { 
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rA << "] + " << imm_s(simm)
        << ";\n";
    out << "    ctx.gpr[" << ppc_inst.rd()
        << "] = (uint32_t)(int32_t)(int16_t)ctx.mmu.read16(ctx.gpr[" << rA
        << "]); // lhau\n";
  } else if (ppc_inst.opcode() == 44) { 
    uint32_t rA = ppc_inst.ra();
    if (rA == 0)
      out << "    ctx.mmu.write16(" << imm_s(ppc_inst.simm()) << ", (uint16_t)ctx.gpr["
          << ppc_inst.rs() << "]); // sth\n";
    else
      out << "    ctx.mmu.write16(ctx.gpr[" << rA << "] + " << imm_s(ppc_inst.simm())
          << ", (uint16_t)ctx.gpr[" << ppc_inst.rs() << "]); // sth\n";
  } else if (ppc_inst.opcode() == 45) { 
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rA << "] + " << imm_s(simm)
        << ";\n";
    out << "    ctx.mmu.write16(ctx.gpr[" << rA << "], (uint16_t)ctx.gpr["
        << ppc_inst.rs() << "]); // sthu\n";
  } else if (ppc_inst.opcode() == 46) { 
    uint32_t rD = ppc_inst.rd();
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    out << "    {\n";
    out << "        uint32_t addr = "
        << (rA == 0 ? std::string("0") : "ctx.gpr[" + std::to_string(rA) + "]")
        << " + " << simm << ";\n";
    out << "        for (int i = " << rD << "; i <= 31; ++i) {\n";
    out << "            ctx.gpr[i] = ctx.mmu.read32(addr);\n";
    out << "            addr += 4;\n";
    out << "        }\n";
    out << "    }\n";
  } else if (ppc_inst.opcode() == 47) { 
    uint32_t rS = ppc_inst.rs();
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    out << "    {\n";
    out << "        uint32_t addr = "
        << (rA == 0 ? std::string("0") : "ctx.gpr[" + std::to_string(rA) + "]")
        << " + " << simm << ";\n";
    out << "        for (int i = " << rS << "; i <= 31; ++i) {\n";
    out << "            ctx.mmu.write32(addr, ctx.gpr[i]);\n";
    out << "            addr += 4;\n";
    out << "        }\n";
    out << "    }\n";
  } else if (ppc_inst.opcode() == 50) { 
    uint32_t rA = ppc_inst.ra();
    if (rA == 0)
      out << "    ctx.fpr[" << ppc_inst.rd() << "] = ctx.mmu.read_f64("
          << imm_s(ppc_inst.simm()) << "); // lfd\n";
    else
      out << "    ctx.fpr[" << ppc_inst.rd() << "] = ctx.mmu.read_f64(ctx.gpr["
          << rA << "] + " << imm_s(ppc_inst.simm()) << "); // lfd\n";
  } else if (ppc_inst.opcode() == 51) { 
    uint32_t rA = ppc_inst.ra();
    out << "    ctx.fpr[" << ppc_inst.rd() << "] = ctx.mmu.read_f64(ctx.gpr["
        << rA << "] + " << ppc_inst.simm() << "); // lfdu\n";
    out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rA << "] + "
        << ppc_inst.simm() << ";\n";
  } else if (ppc_inst.opcode() == 54) { 
    uint32_t rA = ppc_inst.ra();
    if (rA == 0)
      out << "    ctx.mmu.write_f64(" << imm_s(ppc_inst.simm()) << ", ctx.fpr["
          << ppc_inst.rs() << "]); // stfd\n";
    else
      out << "    ctx.mmu.write_f64(ctx.gpr[" << rA << "] + " << imm_s(ppc_inst.simm())
          << ", ctx.fpr[" << ppc_inst.rs() << "]); // stfd\n";
  } else if (ppc_inst.opcode() == 55) { 
    uint32_t rA = ppc_inst.ra();
    out << "    ctx.mmu.write_f64(ctx.gpr[" << rA << "] + " << ppc_inst.simm()
        << ", ctx.fpr[" << ppc_inst.rs() << "]); // stfdu\n";
    out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rA << "] + "
        << ppc_inst.simm() << ";\n";
  } else if (ppc_inst.opcode() == 56) { 
    uint32_t frD = (inst.opcode >> 21) & 0x1F;
    uint32_t rA = (inst.opcode >> 16) & 0x1F;
    uint32_t W = (inst.opcode >> 15) & 0x1;
    uint32_t I = (inst.opcode >> 12) & 0x7;
    int16_t d = inst.opcode & 0xFFF;
    if (d & 0x800)
      d |= 0xF000;
    if (rA == 0)
      out << "    ctx.psq_load(" << frD << ", " << d << ", " << W << ", " << I
          << "); // psq_l\n";
    else
      out << "    ctx.psq_load(" << frD << ", ctx.gpr[" << rA << "] + " << d
          << ", " << W << ", " << I << "); // psq_l\n";
  } else if (ppc_inst.opcode() == 57) { 
    uint32_t frD = (inst.opcode >> 21) & 0x1F;
    uint32_t rA = (inst.opcode >> 16) & 0x1F;
    uint32_t W = (inst.opcode >> 15) & 0x1;
    uint32_t I = (inst.opcode >> 12) & 0x7;
    int16_t d = inst.opcode & 0xFFF;
    if (d & 0x800)
      d |= 0xF000;
    out << "    ctx.psq_load(" << frD << ", ctx.gpr[" << rA << "] + " << d
        << ", " << W << ", " << I << "); // psq_lu\n";
    out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rA << "] + " << d << ";\n";
  } else if (ppc_inst.opcode() == 60) { 
    uint32_t frS = (inst.opcode >> 21) & 0x1F;
    uint32_t rA = (inst.opcode >> 16) & 0x1F;
    uint32_t W = (inst.opcode >> 15) & 0x1;
    uint32_t I = (inst.opcode >> 12) & 0x7;
    int16_t d = inst.opcode & 0xFFF;
    if (d & 0x800)
      d |= 0xF000;
    if (rA == 0)
      out << "    ctx.psq_store(" << frS << ", " << d << ", " << W << ", " << I
          << "); // psq_st\n";
    else
      out << "    ctx.psq_store(" << frS << ", ctx.gpr[" << rA << "] + " << d
          << ", " << W << ", " << I << "); // psq_st\n";
  } else if (ppc_inst.opcode() == 61) { 
    uint32_t frS = (inst.opcode >> 21) & 0x1F;
    uint32_t rA = (inst.opcode >> 16) & 0x1F;
    uint32_t W = (inst.opcode >> 15) & 0x1;
    uint32_t I = (inst.opcode >> 12) & 0x7;
    int16_t d = inst.opcode & 0xFFF;
    if (d & 0x800)
      d |= 0xF000;
    out << "    ctx.psq_store(" << frS << ", ctx.gpr[" << rA << "] + " << d
        << ", " << W << ", " << I << "); // psq_stu\n";
    out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rA << "] + " << d << ";\n";
  } else {
    out << "    nwii::runtime::micro_interpret(ctx, 0x" << std::hex
        << inst.opcode << ", " << pcl(inst.address, false) << std::dec << ");\n";
    out << "    if (ctx.pc != " << pcl(inst.address + 4, false) << std::dec
        << ") return;\n";
  }
}

} 
} 
