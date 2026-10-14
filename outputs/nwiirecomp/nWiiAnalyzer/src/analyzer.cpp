#include "analyzer/analyzer.h"
#include "analyzer/dolphin_sigdb.h"
#include "common/endian.h"
#include "ppc/instruction.h"
#include "analyzer/signature_scanner.h"
#include <iostream>
#include <cctype>
#include <algorithm>
#include <string>
#include <vector>
#include <map>
#include <queue>
#include <algorithm>
#include <cstring>

namespace nwii {
namespace analyzer {

Analyzer::Analyzer(const loader::Executable &executable)
    : executable_(executable) {}

bool Analyzer::read_instruction(uint32_t address, uint32_t &out_inst) const {
  for (const auto &section : executable_.sections) {
    if (section.is_text && address >= section.address &&
        address < section.address + section.size) {
      uint32_t offset = address - section.address;
      if (offset + 4 <= section.data.size()) {
        
        uint32_t raw;
        std::memcpy(&raw, section.data.data() + offset, sizeof(uint32_t));
        out_inst = swap_endian(raw);
        return true;
      }
    }
  }
  return false;
}

bool Analyzer::is_text_address(uint32_t address) const {
  if ((address & 3) != 0)
    return false;
  for (const auto &section : executable_.sections) {
    if (section.is_text && address >= section.address &&
        address < section.address + section.size) {
      return true;
    }
  }
  return false;
}

uint32_t Analyzer::read_data32(uint32_t address) const {
  for (const auto &section : executable_.sections) {
    if (address >= section.address &&
        address < section.address + section.size) {
      uint32_t offset = address - section.address;
      if (offset + 4 <= section.data.size()) {
        uint32_t raw;
        std::memcpy(&raw, section.data.data() + offset, sizeof(uint32_t));
        return swap_endian(raw);
      }
    }
  }
  return 0;
}

void Analyzer::analyze_jump_table(uint32_t bctr_pc,
                                  const std::map<uint32_t, uint32_t> &insts,
                                  std::queue<uint32_t> &block_queue,
                                  std::set<uint32_t> &jump_targets,
                                  const std::set<uint32_t> &known_functions) {
  
  int32_t mtctr_reg = -1;
  uint32_t curr_pc = bctr_pc - 4;

  for (int i = 0; i < 30 && insts.count(curr_pc); ++i, curr_pc -= 4) {
    ppc::Instruction inst(insts.at(curr_pc));

    if (inst.opcode() == 31 && inst.extended_opcode() == 467) {
      uint32_t spr = (inst.value() >> 11) & 0x3FF;
      if (spr == 9) {                            
        mtctr_reg = (inst.value() >> 21) & 0x1F; 
        break;
      }
    }
  }

  if (mtctr_reg == -1)
    return;

  uint32_t table_addr_hi = 0;

  curr_pc = bctr_pc - 4;
  for (int i = 0; i < 30 && insts.count(curr_pc); ++i, curr_pc -= 4) {
    ppc::Instruction inst(insts.at(curr_pc));

    if (inst.opcode() == 15) {
      uint32_t rD = (inst.value() >> 21) & 0x1F;
      uint32_t imm = inst.value() & 0xFFFF;

      table_addr_hi = imm << 16;
      
      break;
    }
  }

  if (table_addr_hi != 0) {

    

    

    uint32_t table_base = table_addr_hi;
    curr_pc = bctr_pc - 4;
    for (int i = 0; i < 30 && insts.count(curr_pc); ++i, curr_pc -= 4) {
      ppc::Instruction inst(insts.at(curr_pc));
      if (inst.opcode() == 14) { 
        int32_t simm = (int16_t)(inst.value() & 0xFFFF);
        table_base += simm;
        break; 
      }
      if (inst.opcode() == 55) { 
        break;
      }
    }

    uint32_t target = read_data32(table_base);
    int valid_targets = 0;

    
    uint32_t scan_addr = table_base;
    while (true) {
      uint32_t ptr = read_data32(scan_addr);
      if (!is_text_address(ptr))
        break;

      int32_t diff = (int32_t)ptr - (int32_t)bctr_pc;
      if (std::abs(diff) > 0x8000)
        break;

      if (known_functions.find(ptr) != known_functions.end()) {
          
          scan_addr += 4;
          continue;
      }

      block_queue.push(ptr);
      jump_targets.insert(ptr);
      valid_targets++;
      scan_addr += 4;

      if (valid_targets > 1000)
        break; 
    }

    if (valid_targets > 0) {
      std::cout << "[Analyzer] Found jump table at 0x" << std::hex << table_base
                << " with " << std::dec << valid_targets
                << " targets from bctr at 0x" << std::hex << bctr_pc << std::dec
                << "\n";
    }
  }
}

void Analyzer::analyze(const std::vector<uint32_t>& additional_roots) {
  std::queue<uint32_t> function_queue;
  std::set<uint32_t> known_functions;

  if (executable_.entry_point != 0) {
    function_queue.push(executable_.entry_point);
    known_functions.insert(executable_.entry_point);
  }

  for (uint32_t root : additional_roots) {
    if (is_text_address(root) &&
        known_functions.find(root) == known_functions.end()) {
      known_functions.insert(root);
      function_queue.push(root);
    }
  }

  for (const auto &section : executable_.sections) {
    if (!section.is_text) {
      for (size_t i = 0; i + 4 <= section.data.size(); i += 4) {
        uint32_t ptr = read_data32(section.address + i);
        if (is_text_address(ptr) &&
            known_functions.find(ptr) == known_functions.end()) {
          known_functions.insert(ptr);
          function_queue.push(ptr);
        }
      }
    } else {

      uint32_t lis_regs[32] = {0};
      for (size_t i = 0; i + 4 <= section.data.size(); i += 4) {
        uint32_t inst = read_data32(section.address + i);
        uint32_t opcode = inst >> 26;
        uint32_t rD = (inst >> 21) & 0x1F;
        uint32_t rA = (inst >> 16) & 0x1F;
        int16_t simm = inst & 0xFFFF;

        if (opcode == 15 && rA == 0) { 
          lis_regs[rD] = (uint32_t)simm << 16;
        } else if (opcode == 14) { 
          if (lis_regs[rA] != 0) {
            uint32_t target = lis_regs[rA] + (int32_t)simm;
            if (is_text_address(target) && known_functions.find(target) == known_functions.end()) {
              known_functions.insert(target);
              function_queue.push(target);
            }
          }
          lis_regs[rD] = 0; 
        } else if (opcode == 18 || opcode == 19 || opcode == 16) {
           
           for (int r = 0; r < 32; r++) lis_regs[r] = 0;
        } else {
           
           lis_regs[rD] = 0;
        }
      }
    }
  }

  std::cout << "[Analyzer] Starting analysis. Entry point: 0x" << std::hex
            << executable_.entry_point << std::dec << "\n";
  int analyzed_count = 0;

  while (!function_queue.empty()) {
    uint32_t func_start = function_queue.front();
    function_queue.pop();

    if (functions_.find(func_start) != functions_.end())
      continue;

    uint32_t min_pc = func_start;
    uint32_t max_pc = func_start;

    std::map<uint32_t, uint32_t> insts;
    std::queue<uint32_t> block_queue;
    std::set<uint32_t> visited_blocks;
    std::set<uint32_t> local_jump_targets;

    block_queue.push(func_start);

    while (!block_queue.empty()) {
      uint32_t current_pc = block_queue.front();
      block_queue.pop();

      while (true) {
        if (visited_blocks.count(current_pc))
          break;
        visited_blocks.insert(current_pc);
        analyzed_blocks_.insert(current_pc);

        uint32_t raw_inst;
        if (!read_instruction(current_pc, raw_inst))
          break;

        insts[current_pc] = raw_inst;
        min_pc = std::min(min_pc, current_pc);
        max_pc = std::max(max_pc, current_pc);

        ppc::Instruction inst(raw_inst);

        if (inst.is_branch_link()) {
          uint32_t target = inst.branch_target(current_pc);
          if (target != 0 &&
              known_functions.find(target) == known_functions.end()) {
            known_functions.insert(target);
            function_queue.push(target);
          }
        } else if (inst.is_unconditional_indirect_branch()) {
          
          if (inst.opcode() == 19 && (inst.value() & 0x3FF) == 528) {
            
            analyze_jump_table(current_pc, insts, block_queue,
                               local_jump_targets, known_functions);
          }
          break;
        } else if (inst.is_unconditional_branch()) {
          uint32_t target = inst.branch_target(current_pc);
          if (target != 0) {
            int32_t diff = (int32_t)target - (int32_t)current_pc;
            if (std::abs(diff) < 0x80000) { 
              block_queue.push(target);
            } else { 
              if (known_functions.find(target) == known_functions.end()) {
                known_functions.insert(target);
                function_queue.push(target);
              }
            }
          }
          break;
        }

        if (inst.opcode() == 16) {
          uint32_t target = inst.branch_target(current_pc);
          if (target != 0) {
            int32_t diff = (int32_t)target - (int32_t)current_pc;
            if (std::abs(diff) < 0x80000) { 
              block_queue.push(target);
            }
          }
        }

        current_pc += 4;
      }
    }

    Function func;
    func.start_address = min_pc;
    func.end_address = max_pc + 4;

    for (const auto &[addr, op] : insts) {
      func.instructions.push_back({addr, op});
    }
    func.jump_table_targets = local_jump_targets;

    functions_[func_start] = func;

    analyzed_count++;
    if (analyzed_count % 1000 == 0) {
      std::cout << "[Analyzer] Analyzed " << analyzed_count
                << " functions. Queue size: " << function_queue.size() << "\n";
    }
  }
  SignatureScanner scanner;
  for (auto &pair : functions_) {
    Function &func = pair.second;
    std::vector<uint32_t> opcodes;
    for (const auto &inst : func.instructions) {
      opcodes.push_back(inst.opcode);
    }
    func.hle_hook_name = scanner.match(opcodes);
    if (!func.hle_hook_name.empty()) {
      std::cout << "[Analyzer] Signature matched at 0x" << std::hex << func.start_address 
                << " -> " << func.hle_hook_name << std::dec << "\n";
    }
  }

  std::cout << "[Analyzer] Analysis complete. Total functions found: "
            << functions_.size() << "\n";
}

int Analyzer::apply_signature_db(const std::string& dsy_path) {
  DolphinSigDB db;
  if (!db.load_dsy(dsy_path)) {
    std::cout << "[Analyzer] No signature DB at " << dsy_path << "\n";
    return 0;
  }

  // Naming has to be trustworthy before anything can be hooked by name, and by
  // default it was not. Measured on the shipped databases: 80% of the names
  // resolved for NFS Hot Pursuit 2 landed on more than one address — `DBClose`
  // alone claimed 257 of them — and names from entirely different games (J3D,
  // nw4hbm, cPlayer) appeared throughout.
  //
  // The cause is the checksum: it is a faithful port of Dolphin's, which hashes
  // opcode bits only and discards every operand. Two functions with the same
  // instruction sequence therefore hash alike, and short functions do so
  // constantly. Dolphin can live with that because a human reads the result;
  // generated hooks cannot.
  //
  // Two rules make it usable. Neither invents information: both only decline to
  // claim what the data does not support.
  //
  //   1. A function must be long enough for its opcode sequence to mean
  //      something. Below this, matches are noise.
  //   2. A name may be used once. If a name matches several addresses, the
  //      signature does not identify a single function and none of them get it.
  constexpr size_t kMinInstructions = 8;

  struct Candidate { uint32_t addr; std::string name; };
  std::vector<Candidate> candidates;
  size_t too_short = 0, size_mismatch = 0, ambiguous_entry = 0;

  for (auto &pair : functions_) {
    Function &func = pair.second;
    if (func.instructions.empty())
      continue;
    if (func.instructions.size() < kMinInstructions) { ++too_short; continue; }

    std::vector<uint32_t> opcodes;
    opcodes.reserve(func.instructions.size());
    for (const auto &inst : func.instructions)
      opcodes.push_back(inst.opcode);

    uint32_t sum = DolphinSigDB::checksum(opcodes);
    const DolphinSigDB::Entry *e = db.match(sum);
    if (!e) continue;
    if (e->ambiguous) { ++ambiguous_entry; continue; }
    if (e->size != (uint32_t)opcodes.size() * 4) { ++size_mismatch; continue; }

    candidates.push_back({pair.first, e->name});
  }

  std::map<std::string, int> uses;
  for (const auto &c : candidates) ++uses[c.name];

  // Disambiguate the rest by library locality.
  //
  // A name that matched several addresses does not identify a function on its
  // own — but the SDK is linked in contiguous blocks, one per object file, so
  // location carries information the checksum lost. The DB records which
  // library each signature came from (the name carries a trailing
  // "\tgx.a GXTexture.o"), and the confident matches pin down where each
  // library sits. A duplicate with exactly one candidate inside its own
  // library's span is then no longer ambiguous.
  //
  // Measured on NFS Hot Pursuit 2: the five GX names that survive uniqueness
  // alone all land in 0x801B8000..0x801BC200, and the four that used to be
  // "found" outside it were the false ones.
  //
  // This still invents nothing. It only uses a fact about how the code was
  // linked, and it only ever narrows a set of candidates the checksum already
  // produced. Repeating the pass lets a widened span settle more names.
  auto library_of = [](const std::string &name) -> std::string {
    const size_t tab = name.find('\t');
    if (tab == std::string::npos) return {};
    size_t b = tab + 1;
    while (b < name.size() && std::isspace((unsigned char)name[b])) ++b;
    size_t e = b;
    while (e < name.size() && !std::isspace((unsigned char)name[e])) ++e;
    return name.substr(b, e - b);
  };

  std::map<uint32_t, std::string> accepted;   // address -> name
  for (const auto &c : candidates)
    if (uses[c.name] == 1) accepted[c.addr] = c.name;

  int by_locality = 0;
  for (int round = 0; round < 8; ++round) {
    struct Span { uint32_t lo, hi; };
    std::map<std::string, Span> spans;
    for (const auto &[addr, name] : accepted) {
      const std::string lib = library_of(name);
      if (lib.empty()) continue;
      auto it = spans.find(lib);
      if (it == spans.end()) spans[lib] = {addr, addr};
      else { it->second.lo = std::min(it->second.lo, addr);
             it->second.hi = std::max(it->second.hi, addr); }
    }

    int gained = 0;
    std::map<std::string, std::vector<uint32_t>> byName;
    for (const auto &c : candidates)
      if (uses[c.name] > 1 && !accepted.count(c.addr))
        byName[c.name].push_back(c.addr);

    for (const auto &[name, addrs] : byName) {
      const std::string lib = library_of(name);
      auto sp = spans.find(lib);
      if (lib.empty() || sp == spans.end()) continue;

      uint32_t only = 0;
      int inside = 0;
      for (uint32_t a : addrs)
        if (a >= sp->second.lo && a <= sp->second.hi) { ++inside; only = a; }

      if (inside == 1) { accepted[only] = name; ++gained; }
    }

    by_locality += gained;
    if (!gained) break;
  }

  int matched = 0;
  for (const auto &[addr, name] : accepted) {
    functions_[addr].sdk_name = name;
    ++matched;
  }
  const int rejected_duplicate = (int)candidates.size() - matched;

  std::cout << "[Analyzer] Signature DB: " << matched << " named, "
            << by_locality << " of them settled by library locality, "
            << rejected_duplicate << " still ambiguous, "
            << ambiguous_entry << " ambiguous in the DB, "
            << size_mismatch << " size mismatch, "
            << too_short << " below " << kMinInstructions << " instructions\n";
  return matched;
}

#if 0
  std::cout << "[Analyzer] Signature DB matched " << matched << " of "
            << functions_.size() << " functions\n";
  return matched;
}
#endif

} 
} 
