#include <cctype>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include "loader/loader.h"
#include "analyzer/analyzer.h"
#include "recompiler/recompiler.h"

#include <toml++/toml.hpp>
#include <filesystem>

namespace {
// PKG10 (roadmap M7.1): relocatable configs. A RELATIVE path value written in
// the config is resolved against the config file's directory, then against
// each ancestor of that directory (so a project-root-relative value such as
// "work/pc-game" works from outputs/pc-profile/ and from copies of the
// profile anywhere below the project root), and otherwise stays relative to
// the current directory (the old behaviour). Inputs must exist at a candidate;
// outputs need only their parent directory (a single-component output name is
// placed next to the config). Absolute values and built-in defaults (keys
// absent from the file) are unchanged; so is --rels-out (a CLI argument stays
// relative to the current directory).
std::string resolve_config_path(const std::string& value,
                                const std::filesystem::path& config_dir,
                                bool is_output) {
    namespace fs = std::filesystem;
    if (value.empty()) return value;
    const fs::path p(value);
    if (p.is_absolute() || p.has_root_name() || p.has_root_directory()) return value;
    std::error_code ec;
    for (fs::path base = config_dir; !base.empty();) {
        const fs::path cand = base / p;
        bool hit = fs::exists(cand, ec);
        if (!hit && is_output)
            hit = p.has_parent_path() ? fs::is_directory(cand.parent_path(), ec)
                                      : base == config_dir;
        if (hit) return cand.lexically_normal().string();
        const fs::path parent = base.parent_path();
        if (parent == base) break;
        base = parent;
    }
    return value;
}
}  // namespace

int main(int argc, char** argv) {
    std::string config_path = (argc >= 2) ? argv[1] : "recomp_config.toml";
    std::filesystem::path config_dir;
    {
        std::error_code ec;
        config_dir = std::filesystem::absolute(std::filesystem::path(config_path), ec).parent_path();
    }
    // Explicit keys only (see resolve_config_path); prints where a relative value went.
    auto resolve = [&](const char* key, const std::string& value, bool is_output) {
        std::string r = resolve_config_path(value, config_dir, is_output);
        if (r != value) std::cout << "[Config] " << key << " = " << value << " -> " << r << "\n";
        return r;
    };

    nwii::recomp::RecompilerConfig config;
    try {
        auto tbl = toml::parse_file(config_path);
        config.project_name = tbl["project_name"].value_or("RecompiledGame");
        config.input_game_dir = tbl["input_game_dir"].value_or(".");
        config.output_dir = tbl["output_dir"].value_or("export");
        config.runtime_source_dir = tbl["runtime_source_dir"].value_or("../nWiiRuntime");
        config.symbols_csv = tbl["symbols_csv"].value_or("");
        if (auto v = tbl["input_game_dir"].value<std::string>())
            config.input_game_dir = resolve("input_game_dir", *v, false);
        if (auto v = tbl["output_dir"].value<std::string>())
            config.output_dir = resolve("output_dir", *v, true);
        if (auto v = tbl["runtime_source_dir"].value<std::string>())
            config.runtime_source_dir = resolve("runtime_source_dir", *v, false);
        if (auto v = tbl["symbols_csv"].value<std::string>())
            config.symbols_csv = resolve("symbols_csv", *v, false);
        config.symbol_prefix = tbl["symbol_prefix"].value_or("");
        
        config.split_output = tbl["split_output"].value_or(false);
        config.instructions_per_file = tbl["instructions_per_file"].value_or(20000);

        if (auto hooks = tbl["hle_hooks"].as_table()) {
            for (const auto& [key, val] : *hooks) {
                uint32_t addr = (uint32_t)std::stoul(std::string(key.str()), nullptr, 16);
                std::string name = val.value_or(std::string());
                if (addr != 0 && !name.empty()) {
                    config.hle_hooks[addr] = name;
                }
            }
            std::cout << "Loaded " << config.hle_hooks.size() << " HLE hooks from config.\n";
        }

        
        if (auto arr = tbl["rel_aot"].as_array()) {
            for (auto& node : *arr) {
                if (auto t = node.as_table()) {
                    nwii::recomp::RecompilerConfig::RelAot r;
                    r.path = (*t)["path"].value_or(std::string());
                    // 0x8051de60 overflows a signed 32-bit int, so read the
                    // bases as int64 before narrowing or value_or returns 0.
                    r.module_base = (uint32_t)(*t)["module_base"].value_or((int64_t)0);
                    r.bss_base = (uint32_t)(*t)["bss_base"].value_or((int64_t)0);
                    if (!r.path.empty() && r.module_base)
                        config.rel_aot.push_back(r);
                }
            }
            std::cout << "Loaded " << config.rel_aot.size()
                      << " REL(s) for AOT recompilation.\n";
        }
        std::cout << "Loaded config from " << config_path << "\n";
    } catch (const toml::parse_error& err) {
        std::cerr << "Config file " << config_path << " not found or invalid.\n";
        return 1;
    }

    std::cout << "NWiiRecomp: loading game from " << config.input_game_dir << "\n";
    nwii::loader::Executable exec;
    if (!exec.load_unpacked_game(config.input_game_dir)) {
        std::cerr << "Failed to load executable.\n";
        return 1;
    }

    std::cout << "Successfully loaded DOL.\n";
    std::cout << "Entry point: 0x" << std::hex << (uint32_t)exec.entry_point << std::dec << "\n";
    std::cout << "Sections loaded: " << exec.sections.size() << "\n";

    
    for (const auto& r : config.rel_aot) {
        std::string p = r.path;
        if (!p.empty() && p[0] != '/')
            p = config.input_game_dir + "/" + p;
        if (exec.load_rel(p, r.module_base, r.bss_base))
            std::cout << "[REL] AOT-loaded " << p << " @0x" << std::hex
                      << r.module_base << " bss 0x" << r.bss_base << std::dec << "\n";
        else
            std::cerr << "[REL] failed to load " << p << "\n";
    }

    nwii::recomp::SymbolTable symbols;
    bool has_symbols = false;
    std::vector<uint32_t> symbol_addresses;

    if (!config.symbols_csv.empty()) {
        std::cout << "Loading symbols from " << config.symbols_csv << "...\n";
        if (symbols.load_csv(config.symbols_csv)) {
            std::cout << "Symbols loaded successfully.\n";
            has_symbols = true;
            for (const auto& [addr, name] : symbols.get_all_symbols()) {
                symbol_addresses.push_back(addr);
            }
        } else {
            std::cerr << "Failed to load symbols.\n";
        }
    }

    std::cout << "Running Code Analyzer...\n";
    nwii::analyzer::Analyzer analyzer(exec);
    analyzer.analyze(symbol_addresses);

    const auto& functions = analyzer.get_functions();
    std::cout << "Analysis complete. Discovered " << functions.size() << " functions.\n";

    // Optional Dolphin signature database (totaldb.dsy): recovers SDK
    
    std::string sigdb_path;
    try {
        auto tbl = toml::parse_file(config_path);
        sigdb_path = tbl["sdk_sigdb"].value_or(std::string());
        sigdb_path = resolve("sdk_sigdb", sigdb_path, false);
    } catch (...) {}
    std::string sdk_report;
    if (!sigdb_path.empty()) {
        if (analyzer.apply_signature_db(sigdb_path) > 0) {
            sdk_report = "Name,Location,Size\n";
            char line[192];
            for (const auto& [addr, func] : functions) {
                if (func.sdk_name.empty())
                    continue;
                uint32_t size = (uint32_t)func.instructions.size() * 4;
                std::snprintf(line, sizeof(line), "\"%s\",%08X,%u\n",
                              func.sdk_name.c_str(), addr, size);
                sdk_report += line;

                
                std::string ident = func.sdk_name;
                for (char& c : ident)
                    if (!std::isalnum((unsigned char)c) && c != '_')
                        c = '_';
                char suffix[16];
                std::snprintf(suffix, sizeof(suffix), "_%08X", addr);
                symbols.add_symbol(addr, ident + suffix);
            }
            has_symbols = true;
        }
    }

    // RELAOT (M2.7): `nwiirecomp <config> --rels [--rels-out DIR]` emits the
    // REL modules listed in [rels] (position-independent C++) instead of the
    // DOL export. The DOL analysis above is still needed: REL code calls DOL
    // functions by the same emitted names as the export.
    bool rels_mode = false;
    for (int a = 2; a < argc; ++a)
        if (std::string(argv[a]) == "--rels") rels_mode = true;
    if (rels_mode) {
        nwii::recomp::Recompiler::RelGenOptions ro;
        try {
            auto tbl = toml::parse_file(config_path);
            auto r = tbl["rels"];
            ro.rel_dir = resolve("rels.rel_dir", r["rel_dir"].value_or(std::string()), false);
            ro.symbols_dir = resolve("rels.symbols_dir", r["symbols_dir"].value_or(std::string()), false);
            ro.output_dir = resolve("rels.output_dir", r["output_dir"].value_or(std::string()), true);
            ro.functions_h = resolve("rels.functions_h", r["functions_h"].value_or(std::string()), false);
            if (auto arr = r["modules"].as_array())
                for (auto& e : *arr)
                    if (auto s = e.value<std::string>()) ro.modules.push_back(*s);
        } catch (const toml::parse_error& err) {
            std::cerr << "[RELAOT] config: " << err << "\n";
            return 1;
        }
        for (int a = 2; a + 1 < argc; ++a)
            if (std::string(argv[a]) == "--rels-out") ro.output_dir = argv[a + 1];
        nwii::recomp::Recompiler recompiler(analyzer, has_symbols ? &symbols : nullptr, config);
        return recompiler.generate_rels(ro) ? 0 : 1;
    }

    std::cout << "Generating CMake Project...\n";
    nwii::recomp::Recompiler recompiler(analyzer, has_symbols ? &symbols : nullptr, config);
    if (recompiler.generate_cmake_project(exec.entry_point)) {
        std::cout << "Successfully exported standalone project to '" << config.output_dir << "' directory!\n";
        if (!sdk_report.empty()) {
            std::ofstream report(config.output_dir + "/sdk_symbols.csv");
            report << sdk_report;
            std::cout << "SDK symbol report: " << config.output_dir
                      << "/sdk_symbols.csv\n";
        }
    } else {
        std::cerr << "Failed to export project.\n";
    }

    return 0;
}

