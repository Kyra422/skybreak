// RELAOT (roadmap M2.7): position-independent, relocation-aware recompilation
// of REL modules (OSModule v1-3) into C++.
//
// Input: the original .rel bytes (never patched memory) and the decomp's
// per-module symbols.txt (exact function extents). Output per module one TU
// rel_<id>.cpp, plus rel_aot_table.cpp (module list + DOL entry table).
//
// Code model (runtime side: nWiiRuntime include/runtime/rel_aot.h):
//   * instruction addresses are emitted as virtual addresses va_base + off;
//     every PC literal prints as (SB + off) with SB = the section's runtime
//     base (set when the game's OSLink has linked the module);
//   * a word covered by a relocation reads its patched immediate / branch
//     displacement from guest RAM (SH + off) at run time;
//   * b/bl REL24 to the DOL or to this module calls the recompiled function
//     directly when the patched target equals the relocation's target, else
//     relaot::call() resolves it at run time; bctr inside the function goes
//     through the function's own entry switch; bctrl uses relaot::call().
#include "recompiler/recompiler.h"
#include "ppc/instruction.h"
#include "runtime/rel_aot.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>

namespace nwii {
namespace recomp {

namespace {

uint32_t be32(const std::vector<uint8_t> &d, size_t o) {
  return (uint32_t)d[o] << 24 | (uint32_t)d[o + 1] << 16 | (uint32_t)d[o + 2] << 8 | d[o + 3];
}
uint16_t be16(const std::vector<uint8_t> &d, size_t o) {
  return (uint16_t)(d[o] << 8 | d[o + 1]);
}

struct RelSection {
  uint32_t offset, size;
  bool exec;
};
struct RelEntry {
  uint32_t module;
  uint8_t sec;
  uint32_t off;
  uint8_t type;
  uint8_t tsec;
  uint32_t addend;
};
struct RelModule {
  std::string name;
  std::vector<uint8_t> data;
  uint32_t id = 0, version = 0;
  std::vector<RelSection> sections;
  std::vector<RelEntry> relocs;
  bool parse(std::string &err) {
    if (data.size() < 0x40) {
      err = "file too small";
      return false;
    }
    id = be32(data, 0);
    const uint32_t nsec = be32(data, 0x0C), sinfo = be32(data, 0x10);
    version = be32(data, 0x1C);
    const uint32_t imp_off = be32(data, 0x28), imp_size = be32(data, 0x2C);
    if (sinfo + 8ull * nsec > data.size() || imp_off + (uint64_t)imp_size > data.size()) {
      err = "bad header";
      return false;
    }
    for (uint32_t s = 0; s < nsec; ++s) {
      const uint32_t o = be32(data, sinfo + 8 * s), z = be32(data, sinfo + 8 * s + 4);
      sections.push_back({o & ~1u, z, (o & 1) != 0});
    }
    for (uint32_t k = 0; k + 8 <= imp_size; k += 8) {
      const uint32_t mod = be32(data, imp_off + k), off = be32(data, imp_off + k + 4);
      int sec = -1;
      uint32_t pos = 0;
      for (size_t o = off; o + 8 <= data.size(); o += 8) {
        const uint16_t d = be16(data, o);
        const uint8_t type = data[o + 2], tsec = data[o + 3];
        const uint32_t addend = be32(data, o + 4);
        if (type == 203)
          break; // R_DOLPHIN_END
        if (type == 202) { // R_DOLPHIN_SECTION
          sec = tsec;
          pos = 0;
          continue;
        }
        pos += d;
        if (type == 0 || type == 201) // NONE / R_DOLPHIN_NOP
          continue;
        if (sec < 0) {
          err = "relocation before section";
          return false;
        }
        relocs.push_back({mod, (uint8_t)sec, pos, type, tsec, addend});
      }
    }
    return true;
  }
};

struct RelSym {
  std::string name;
  uint32_t addr, size;
};

std::vector<RelSym> load_text_functions(const std::string &path) {
  std::vector<RelSym> out;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    const size_t eq = line.find(" = .text:0x");
    if (eq == std::string::npos || line.find("type:function") == std::string::npos)
      continue;
    const size_t sz = line.find("size:0x");
    if (sz == std::string::npos)
      continue;
    RelSym s;
    s.name = line.substr(0, eq);
    s.addr = (uint32_t)std::stoul(line.substr(eq + 11), nullptr, 16);
    s.size = (uint32_t)std::stoul(line.substr(sz + 7), nullptr, 16);
    out.push_back(s);
  }
  std::sort(out.begin(), out.end(), [](const RelSym &a, const RelSym &b) { return a.addr < b.addr; });
  return out;
}

std::string hex8(uint32_t v) {
  std::ostringstream ss;
  ss << std::hex << std::uppercase << std::setfill('0') << std::setw(8) << v;
  return ss.str();
}

} // namespace

bool Recompiler::emit_rel_branch(std::ostream &out, const analyzer::Instruction &inst,
                                 const RelReloc &r) {
  const uint32_t off = inst.address - rel_->va_base;
  if (r.type != 10) { // not R_PPC_REL24: execute the patched word itself
    ++rel_->n_reloc_micro;
    out << "    nwii::runtime::micro_interpret(ctx, nwii::runtime::relaot::rd32(SH + 0x"
        << std::hex << std::uppercase << off << std::dec << "u), " << pcl(inst.address) << ");\n";
    out << "    if (ctx.pc != " << pcl(inst.address + 4) << ") return;\n";
    return true;
  }
  ++rel_->n_reloc_branch;
  const bool lk = (inst.opcode & 1) != 0;
  std::string name, expect;
  bool self = false;
  if (r.module == 0) {
    const auto it = emitted_names_.find(r.addend);
    if (it != emitted_names_.end())
      name = it->second;
    expect = "0x" + hex8(r.addend) + "u";
  } else if (r.module == rel_->module_id && r.tsec == rel_->section) {
    const auto it = rel_->funcs.find(r.addend);
    if (it != rel_->funcs.end())
      name = it->second;
    std::ostringstream e;
    e << pcl(rel_->va_base + r.addend);
    expect = e.str();
    self = true;
  }
  out << "    {\n";
  out << "        const uint32_t t = nwii::runtime::relaot::branch_target("
         "nwii::runtime::relaot::rd32(SH + 0x"
      << std::hex << std::uppercase << off << std::dec << "u), " << pcl(inst.address) << ");\n";
  if (lk) {
    out << "        ctx.lr = " << pcl(inst.address + 4) << ";\n";
    if (!name.empty()) {
      ++(self ? rel_->n_direct_self : rel_->n_direct_dol);
      out << "        if (t == " << expect << ") { ctx.pc = t; " << name
          << "(ctx); if (ctx.pc != " << pcl(inst.address + 4) << ") return; }\n";
      out << "        else if (!nwii::runtime::relaot::call(ctx, t, " << pcl(inst.address + 4)
          << ")) return;\n";
    } else {
      ++rel_->n_generic;
      out << "        if (!nwii::runtime::relaot::call(ctx, t, " << pcl(inst.address + 4)
          << ")) return;\n";
    }
  } else {
    if (!name.empty() && !self) {
      // tail call into the DOL: native (the DOL's own b goes through the
      // dispatcher, so this cannot recurse without bound)
      ++rel_->n_direct_dol;
      out << "        ctx.pc = t;\n";
      out << "        if (t == " << expect << ") { " << name << "(ctx); return; }\n";
      out << "        return;\n";
    } else {
      ++rel_->n_generic;
      out << "        ctx.pc = t; return;\n";
    }
  }
  out << "    }\n";
  return true;
}

void Recompiler::emit_rel_function(std::ostream &out, const analyzer::Function &func,
                                   const std::string &func_name) {
  const uint32_t va = rel_->va_base;
  out << "static void " << func_name << "(CPUContext& ctx) {\n";
  out << "    const uint32_t SB = s_sb; const uint8_t* const SH = s_sh;\n";
  out << "    int32_t RIMM = 0; (void)RIMM; (void)SH;\n";
  out << "    if (ctx.pc != (SB + 0x" << std::hex << std::uppercase
      << (func.start_address - va) << std::dec << "u)) {\n";
  out << "    rel_dispatch:\n";
  out << "        switch (ctx.pc - SB) {\n";
  // Only the real entry points (see entry_points() in generate_rels): every
  // extra case label is a potential CFG entry and costs compile time.
  for (const uint32_t e : rel_->entries.at(func.start_address - va))
    out << "            case 0x" << std::hex << std::uppercase << e << "u: goto loc_" << (va + e)
        << std::dec << ";\n";
  out << "            default: return; // elsewhere: the REL loop / interpreter routes ctx.pc\n";
  out << "        }\n";
  out << "    }\n";
  // Unlike the DOL emitter, the instruction count is bumped after the label,
  // so a taken branch or a mid-function (re)entry counts its target exactly
  // like the REL interpreter (one per executed instruction): moving a module
  // from the interpreter to AOT does not change the virtual timebase.
  for (const auto &inst : func.instructions) {
    std::ostringstream one;
    const uint32_t off = inst.address - va;
    const bool imm_reloc = rel_->relocs.count(off) && (inst.opcode >> 26) != 18;
    const ppc::Instruction pi(inst.opcode);
    // bctr / bctrl (unconditional): stay native where possible
    if (pi.opcode() == 19 && pi.extended_opcode() == 528 && (pi.bo() & 0x14) == 0x14 &&
        !rel_->relocs.count(off)) {
      one << "loc_" << std::hex << std::uppercase << inst.address << std::dec << ": ;\n";
      one << "    ++ctx.inst_count;\n";
      if (pi.lk()) {
        one << "    ctx.lr = " << pcl(inst.address + 4) << "; // bctrl\n";
        // no null-call guard (unlike the DOL emitter): a call to 0 goes to
        // the dispatcher exactly as the REL interpreter's bctrl did
        one << "    if (!nwii::runtime::relaot::call(ctx, ctx.ctr & ~3u, " << pcl(inst.address + 4)
            << ")) return;\n";
      } else {
        one << "    ctx.pc = ctx.ctr & ~3u; goto rel_dispatch; // bctr\n";
      }
      out << one.str();
      continue;
    }
    emit_instruction(one, inst, func);
    std::string text = one.str();
    text.insert(text.find('\n') + 1, "    ++ctx.inst_count;\n"); // after "loc_X: ;"
    if (imm_reloc && text.find("RIMM =") != std::string::npos &&
        text.find("RIMM", text.find("RIMM =") + 6) == std::string::npos) {
      std::cerr << "[RELAOT] relocated immediate not consumed at module " << rel_->module_id
                << " off 0x" << std::hex << off << std::dec << "\n";
      std::exit(3);
    }
    out << text;
  }
  // falls off the end (e.g. after a call that does not return): continue at
  // the next word through the REL dispatch loop
  const uint32_t end = func.instructions.empty() ? func.start_address
                                                 : func.instructions.back().address + 4;
  out << "    ctx.pc = " << pcl(end) << "; return;\n";
  out << "}\n\n";
}

bool Recompiler::generate_rels(const RelGenOptions &o) {
  namespace fs = std::filesystem;
  build_emitted_names();
  std::set<std::string> export_names;
  if (!o.functions_h.empty()) {
    std::ifstream in(o.functions_h);
    std::string line;
    while (std::getline(in, line)) {
      if (line.rfind("void ", 0) == 0) {
        const size_t p = line.find('(');
        if (p != std::string::npos)
          export_names.insert(line.substr(5, p - 5));
      }
    }
    std::cout << "[RELAOT] export functions.h: " << export_names.size() << " names\n";
  }
  std::vector<std::string> stems;
  const bool all = o.modules.empty() ||
                   std::find(o.modules.begin(), o.modules.end(), "all") != o.modules.end();
  if (all) {
    for (const auto &e : fs::directory_iterator(o.rel_dir))
      if (e.path().extension() == ".rel")
        stems.push_back(e.path().stem().string());
    std::sort(stems.begin(), stems.end());
  } else {
    stems = o.modules;
  }
  fs::create_directories(o.output_dir);
  struct Done {
    uint32_t id;
    std::string name;
  };
  std::vector<Done> done;
  std::set<std::string> dol_used;
  uint64_t tot_funcs = 0, tot_insts = 0, tot_entries = 0;
  RelEmitCtx totals;
  std::ofstream report(o.output_dir + "/rel_aot_report.csv");
  report << "module,id,functions,instructions,reloc_branch,reloc_imm,reloc_micro,"
            "direct_dol,direct_self,generic\n";
  for (const auto &stem : stems) {
    RelModule m;
    m.name = stem;
    {
      std::ifstream in(o.rel_dir + "/" + stem + ".rel", std::ios::binary);
      if (!in) {
        std::cerr << "[RELAOT] missing " << stem << ".rel\n";
        return false;
      }
      m.data.assign(std::istreambuf_iterator<char>(in), {});
    }
    std::string err;
    if (!m.parse(err)) {
      std::cerr << "[RELAOT] " << stem << ": " << err << "\n";
      return false;
    }
    const auto syms = load_text_functions(o.symbols_dir + "/" + stem + "/symbols.txt");
    std::vector<uint32_t> exec;
    for (uint32_t s = 0; s < m.sections.size(); ++s)
      if (m.sections[s].exec && m.sections[s].offset && m.sections[s].size)
        exec.push_back(s);
    if (exec.size() != 1 || syms.empty()) {
      std::cerr << "[RELAOT] " << stem << ": " << exec.size()
                << " executable sections / " << syms.size()
                << " .text functions; only one .text is supported\n";
      return false;
    }
    const uint32_t sec = exec[0];
    const RelSection &S = m.sections[sec];
    RelEmitCtx rc;
    rc.module_id = m.id;
    rc.section = sec;
    rc.prefix = "rel" + std::to_string(m.id) + "_";
    for (const auto &r : m.relocs) {
      if (r.sec != sec)
        continue;
      const uint32_t w = r.off & ~3u;
      if (rc.relocs.count(w)) {
        std::cerr << "[RELAOT] " << stem << ": two relocations on word 0x" << std::hex << w
                  << std::dec << "\n";
        return false;
      }
      // NEGATIVE CONTROL for the differential harness only: emit the file's
      // unrelocated words as if they were position-independent code.
      static const bool negative_control = std::getenv("NWII_RELGEN_NEGATIVE_CONTROL") != nullptr;
      if (!negative_control)
        rc.relocs[w] = RelReloc{r.module, r.type, r.tsec, r.addend};
      // every relocated word must be masked by image_hash()
      const uint32_t word = be32(m.data, S.offset + w);
      const uint32_t op = word >> 26;
      const bool masked = op == 18 || op == 13 || op == 14 || op == 15 || (op >= 32 && op <= 55);
      if (!masked) {
        std::cerr << "[RELAOT] " << stem << ": relocated opcode " << op << " at 0x" << std::hex
                  << w << std::dec << " is not hash-masked\n";
        return false;
      }
    }
    std::vector<analyzer::Function> funcs;
    for (const auto &s : syms) {
      if (s.addr % 4 || s.size % 4 || s.addr + s.size > S.size || !s.size) {
        std::cerr << "[RELAOT] " << stem << ": bad symbol " << s.name << "\n";
        return false;
      }
      analyzer::Function f;
      f.start_address = rc.va_base + s.addr;
      f.end_address = rc.va_base + s.addr + s.size;
      for (uint32_t a = s.addr; a < s.addr + s.size; a += 4)
        f.instructions.push_back({rc.va_base + a, be32(m.data, S.offset + a)});
      rc.funcs[s.addr] = rc.prefix + hex8(s.addr);
      funcs.push_back(std::move(f));
    }
    // Entry points per function (section offsets). A native function can only
    // be (re)entered where some path leaves ctx.pc pointing into it:
    //   start; after every LK branch (return through the dispatcher); the
    //   address of a backward local branch (its poll may take an interrupt);
    //   after sc / mtmsr (their polls); targets of direct branches from other
    //   functions; and every text address the module materialises through a
    //   relocation (jump tables, vtables, function pointers, lis/addi pairs).
    // Any other pc is handed to the interpreter by relaot::lookup().
    {
      static const bool all_entries = std::getenv("NWII_RELGEN_ALL_ENTRIES") != nullptr;
      auto func_of = [&](uint32_t off) -> int {
        auto it = std::upper_bound(syms.begin(), syms.end(), off,
                                   [](uint32_t v, const RelSym &s) { return v < s.addr; });
        if (it == syms.begin())
          return -1;
        --it;
        return off < it->addr + it->size ? (int)(it - syms.begin()) : -1;
      };
      std::vector<std::set<uint32_t>> ent(syms.size());
      for (size_t i = 0; i < syms.size(); ++i) {
        const RelSym &s = syms[i];
        ent[i].insert(s.addr);
        for (uint32_t off = s.addr; off < s.addr + s.size; off += 4) {
          if (all_entries) {
            ent[i].insert(off);
            continue;
          }
          const uint32_t w = be32(m.data, S.offset + off), op = w >> 26;
          const uint32_t xo = (w >> 1) & 0x3FF;
          const bool lk = (w & 1) != 0;
          if (op == 16 || op == 18) {
            if (lk && off + 4 < s.addr + s.size)
              ent[i].insert(off + 4);
            if (!(w & 2) && !rc.relocs.count(off)) {
              int32_t d = op == 18 ? (int32_t)(w & 0x03FFFFFC) : (int32_t)(int16_t)(w & 0xFFFC);
              if (op == 18 && (d & 0x02000000))
                d -= 0x04000000;
              const uint32_t t = off + (uint32_t)d;
              const int j = func_of(t);
              if (j == (int)i && t <= off)
                ent[i].insert(off); // back-edge: its poll stores this pc
              else if (j >= 0 && j != (int)i)
                ent[j].insert(t);
            }
          } else if (op == 19 && (xo == 16 || xo == 528) && lk) {
            if (off + 4 < s.addr + s.size)
              ent[i].insert(off + 4);
          } else if (op == 17 || (op == 31 && xo == 146)) { // sc, mtmsr
            if (off + 4 < s.addr + s.size)
              ent[i].insert(off + 4);
          }
        }
      }
      for (const auto &r : m.relocs)
        if (r.module == m.id && r.tsec == sec) {
          const int j = func_of(r.addend & ~3u);
          if (j >= 0)
            ent[j].insert(r.addend & ~3u);
        }
      for (size_t i = 0; i < syms.size(); ++i)
        rc.entries[syms[i].addr] = ent[i];
    }
    const uint64_t hash =
        nwii::runtime::relaot::image_hash(m.data.data() + S.offset, S.size);

    const std::string path = o.output_dir + "/rel_" + std::to_string(m.id) + ".cpp";
    std::ostringstream body;
    rel_ = &rc;
    for (size_t i = 0; i < funcs.size(); ++i) {
      body << "// " << syms[i].name << "\n";
      // PERF3: huge functions use the MMU's out-of-line access forms (compile time)
      std::ostringstream fn;
      emit_rel_function(fn, funcs[i], rc.funcs[syms[i].addr]);
      std::string text = fn.str();
      outline_memory_accesses(text);
      body << text;
    }
    rel_ = nullptr;
    std::ofstream out(path);
    out << "// RELAOT: " << stem << " (REL id " << m.id << ", section " << sec << ", 0x"
        << std::hex << S.size << std::dec << " bytes). Generated by nwiirecomp --rels.\n";
    out << "#include \"runtime/cpu_context.h\"\n#include \"runtime/config.h\"\n"
           "#include \"runtime/rel_aot.h\"\n#include <stdint.h>\n#include <bit>\n"
           "#include <cmath>\n#include <iostream>\n#include <cstdlib>\n";
    out << "#include \"functions.h\"\n\nusing namespace nwii::runtime;\n\n";
    out << "namespace {\nuint32_t s_sb = 0;\nconst uint8_t* s_sh = nullptr;\n}\n\n";
    for (const auto &[a, n] : rc.funcs)
      out << "static void " << n << "(CPUContext& ctx);\n";
    out << "\n" << body.str();
    out << "static const uint32_t k_entries[] = {";
    size_t n_ent = 0;
    for (const auto &s : syms)
      for (const uint32_t e : rc.entries[s.addr]) {
        out << ((n_ent++ % 12) ? " " : "\n    ") << "0x" << std::hex << std::uppercase << e << "u,"
            << std::dec;
      }
    out << "\n};\n";
    out << "static const nwii::runtime::relaot::FuncEntry k_funcs[] = {\n";
    size_t first = 0;
    for (const auto &s : syms) {
      const size_t n = rc.entries[s.addr].size();
      out << "    {0x" << std::hex << std::uppercase << s.addr << "u, 0x" << (s.addr + s.size)
          << "u, " << std::dec << rc.funcs[s.addr] << ", " << first << ", " << n << "},\n";
      first += n;
    }
    out << "};\n";
    out << "static const nwii::runtime::relaot::SectionDesc k_secs[] = {\n    {" << sec << ", 0x"
        << std::hex << S.size << "u, 0x" << hash << "ull, k_funcs, " << std::dec << syms.size()
        << ", &s_sb, &s_sh, k_entries},\n};\n";
    tot_entries += n_ent;
    out << "namespace nwii { namespace runtime { namespace relaot {\n";
    out << "extern const ModuleDesc relaot_mod_" << m.id << ";\n";
    out << "const ModuleDesc relaot_mod_" << m.id << " = {" << m.id << ", \"" << stem
        << "\", k_secs, 1};\n";
    out << "} } }\n";
    out.close();

    // DOL names referenced by direct calls: must exist in the export
    for (const auto &r : rc.relocs)
      if (r.second.module == 0 && r.second.type == 10) {
        const auto it = emitted_names_.find(r.second.addend);
        if (it != emitted_names_.end())
          dol_used.insert(it->second);
      }
    tot_funcs += funcs.size();
    for (const auto &f : funcs)
      tot_insts += f.instructions.size();
    report << stem << "," << m.id << "," << funcs.size() << "," << S.size / 4 << ","
           << rc.n_reloc_branch << "," << rc.n_reloc_imm << "," << rc.n_reloc_micro << ","
           << rc.n_direct_dol << "," << rc.n_direct_self << "," << rc.n_generic << "\n";
    totals.n_reloc_branch += rc.n_reloc_branch;
    totals.n_reloc_imm += rc.n_reloc_imm;
    totals.n_reloc_micro += rc.n_reloc_micro;
    totals.n_direct_dol += rc.n_direct_dol;
    totals.n_direct_self += rc.n_direct_self;
    totals.n_generic += rc.n_generic;
    done.push_back({m.id, stem});
  }
  if (!export_names.empty()) {
    size_t missing = 0;
    for (const auto &n : dol_used)
      if (!export_names.count(n)) {
        if (missing++ < 10)
          std::cerr << "[RELAOT] DOL name not in export functions.h: " << n << "\n";
      }
    size_t missing_tab = 0;
    for (const auto &[a, n] : emitted_names_)
      if (!export_names.count(n))
        ++missing_tab;
    if (missing || missing_tab) {
      std::cerr << "[RELAOT] export does not match this DOL analysis (" << missing << " call / "
                << missing_tab << " table names missing); regenerate both together\n";
      return false;
    }
  }
  // table: modules + DOL entry points (sorted by address: emitted_names_ is a map)
  std::ofstream t(o.output_dir + "/rel_aot_table.cpp");
  t << "// RELAOT module table. Generated by nwiirecomp --rels.\n";
  t << "#include \"runtime/cpu_context.h\"\n#include \"runtime/rel_aot.h\"\n#include \"functions.h\"\n\n";
  t << "namespace nwii { namespace runtime { namespace relaot {\n";
  for (const auto &d : done)
    t << "extern const ModuleDesc relaot_mod_" << d.id << ";\n";
  t << "extern const ModuleDesc* const g_modules[] = {\n";
  for (const auto &d : done)
    t << "    &relaot_mod_" << d.id << ", // " << d.name << "\n";
  if (done.empty())
    t << "    nullptr\n";
  t << "};\n";
  t << "extern const uint32_t g_module_count = " << done.size() << ";\n";
  t << "extern const DolFn g_dol_funcs[] = {\n";
  for (const auto &[a, n] : emitted_names_)
    t << "    {0x" << hex8(a) << "u, &" << n << "},\n";
  t << "};\n";
  t << "extern const uint32_t g_dol_func_count = " << emitted_names_.size() << ";\n";
  t << "} } }\n";
  std::cout << "[RELAOT] generated " << done.size() << " modules, " << tot_funcs << " functions, "
            << tot_insts << " instructions, " << tot_entries << " entry points; reloc branches " << totals.n_reloc_branch
            << " (direct DOL " << totals.n_direct_dol << ", direct self " << totals.n_direct_self
            << ", generic " << totals.n_generic << "), reloc immediates " << totals.n_reloc_imm
            << ", reloc micro " << totals.n_reloc_micro << "\n";
  return true;
}

} // namespace recomp
} // namespace nwii
