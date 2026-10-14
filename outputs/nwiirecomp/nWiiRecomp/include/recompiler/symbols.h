#pragma once

#include <string>
#include <map>
#include <set>
#include <cstdint>

namespace nwii {
namespace recomp {

class SymbolTable {
public:
    bool load_csv(const std::string& filepath);
    
    std::string get_symbol(uint32_t address) const;
    bool has_symbol(uint32_t address) const;
    bool has_authoritative_symbol(uint32_t address) const;
    void add_symbol(uint32_t address, const std::string& name);
    const std::map<uint32_t, std::string>& get_all_symbols() const { return symbols_; }

private:
    std::map<uint32_t, std::string> symbols_;
    std::set<uint32_t> authoritative_addresses_;
};

} 
} 
