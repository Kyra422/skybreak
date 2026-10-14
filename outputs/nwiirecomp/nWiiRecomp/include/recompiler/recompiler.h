#pragma once
#include "analyzer/analyzer.h"
#include "recompiler/symbols.h"
#include <iosfwd>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace nwii {
namespace recomp {

struct RecompilerConfig {
    std::string project_name = "RecompiledGame";
    std::string input_game_dir = "";
    std::string output_dir = "export";
    std::string runtime_source_dir = "../nWiiRuntime";
    std::string symbols_csv = "";
    // Prefix used to avoid host-library name collisions in a supplied symbol map.
    std::string symbol_prefix = "";

    bool split_output = false;
    int instructions_per_file = 20000;

    

    std::map<uint32_t, std::string> hle_hooks;

    

    struct RelAot { std::string path; uint32_t module_base; uint32_t bss_base; };
    std::vector<RelAot> rel_aot;
};

class Recompiler {
public:
    Recompiler(const analyzer::Analyzer& analyzer, const SymbolTable* symbols = nullptr, const RecompilerConfig& config = RecompilerConfig());

    std::vector<std::string> generate_cpp(uint32_t entry_point);

    bool generate_cmake_project(uint32_t entry_point);

    std::string generate_function_cpp(const analyzer::Function& func);

    // RELAOT (M2.7): position-independent recompilation of REL modules.
    // Implemented in rel_gen.cpp. Needs the DOL analysis of this instance
    // (direct calls into the DOL use the same emitted names as the export).
    struct RelGenOptions {
        std::string rel_dir;        // extracted *.rel files
        std::string symbols_dir;    // decomp config/<GAME>/rels/<name>/symbols.txt
        std::string output_dir;     // rel_<id>.cpp + rel_aot_table.cpp
        std::vector<std::string> modules; // module file stems; empty or "all" = every .rel
        std::string functions_h;    // export functions.h to check DOL names against ("" = skip)
    };
    bool generate_rels(const RelGenOptions& opts);

    // Code model of the REL being emitted; null in DOL mode. When set,
    // instruction addresses are virtual (va_base + section offset) and
    // every PC literal is printed as (SB + offset).
    struct RelReloc { uint32_t module; uint8_t type; uint8_t tsec; uint32_t addend; };
    struct RelEmitCtx {
        uint32_t module_id = 0;
        uint32_t section = 0;
        uint32_t va_base = 0x40000000u;
        std::string prefix;                       // "rel557_"
        std::map<uint32_t, RelReloc> relocs;      // word offset -> reloc
        std::map<uint32_t, std::string> funcs;    // start offset -> emitted name
        std::map<uint32_t, std::set<uint32_t>> entries; // start offset -> entry offsets (switch cases)
        mutable uint64_t n_reloc_branch = 0, n_reloc_imm = 0, n_reloc_micro = 0;
        mutable uint64_t n_direct_dol = 0, n_direct_self = 0, n_generic = 0;
    };
    // Prints a PC literal: DOL "0x<addr>" (exactly as the DOL emitter always
    // did), REL "(SB + 0x<off>u)".
    struct PcLit { const RelEmitCtx* rel; uint32_t v; bool upper; };
    PcLit pcl(uint32_t v, bool upper = true) const { return PcLit{rel_, v, upper}; }
    // Immediate operand of a D-form instruction: the file value in DOL mode,
    // the runtime-read relocated value (RIMM) for a relocated REL word.
    struct ImmS { int16_t v; bool rel; };
    struct ImmHi { int16_t v; bool rel; };   // simm << 16 (addis)
    struct ImmX { uint32_t v; bool rel; };   // (uint32_t)(int32_t)simm printed 0x..ull

private:
    ImmS imm_s(int16_t v) const { return ImmS{v, imm_override_}; }
    ImmHi imm_hi(int16_t v) const { return ImmHi{v, imm_override_}; }
    ImmX imm_x(uint32_t v) const { return ImmX{v, imm_override_}; }
    void emit_rel_function(std::ostream& out, const analyzer::Function& func, const std::string& func_name);
    bool emit_rel_branch(std::ostream& out, const analyzer::Instruction& inst, const RelReloc& r);
    const RelEmitCtx* rel_ = nullptr;
    bool imm_override_ = false;
    void emit_function(std::ostream& out, const analyzer::Function& func, const std::string& func_name);
    void emit_function_body(std::ostream& out, const analyzer::Function& func, const std::string& func_name);
    // PERF3: in a function with more than kOutlineMemSites guest memory
    // accesses, use the MMU's out-of-line (_ol) access forms (compile time).
    static constexpr size_t kOutlineMemSites = 200;
    static void outline_memory_accesses(std::string& fn_text);
    void emit_instruction(std::ostream& out, const analyzer::Instruction& inst, const analyzer::Function& func);
    // Identifier a direct branch/call to `target` must use, or "" when no
    // recompiled function starts there (the branch then goes through the
    // dispatcher, which switches on the address).
    std::string direct_target_name(uint32_t target) const;
    // Fills emitted_names_ with the identifier every analyzed function is
    // emitted under (sanitised symbol, uniquified with _<ADDR> on collision).
    void build_emitted_names();

    const analyzer::Analyzer& analyzer_;
    const SymbolTable* symbols_;
    RecompilerConfig config_;
    // start address -> emitted identifier; the single source of truth for
    // function definitions, declarations and every direct call site.
    std::map<uint32_t, std::string> emitted_names_;
    std::vector<std::string> base_names_;  // seam-match name per function, same order
    mutable uint64_t direct_calls_ = 0;     // direct calls/tail branches emitted by name
    mutable uint64_t dispatcher_branches_ = 0; // non-local branches with no function at target
};
// RELAOT: printers for Recompiler::PcLit / ImmS / ImmHi / ImmX (recompiler.cpp).
std::ostream& operator<<(std::ostream& os, const Recompiler::PcLit& p);
std::ostream& operator<<(std::ostream& os, const Recompiler::ImmS& s);
std::ostream& operator<<(std::ostream& os, const Recompiler::ImmHi& s);
std::ostream& operator<<(std::ostream& os, const Recompiler::ImmX& s);


} 
} 
