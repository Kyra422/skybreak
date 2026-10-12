#include "runtime/mods.h"
#include "runtime/gx/dolphin_texname.h"
#include <toml++/toml.hpp>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace nwii::runtime::mods {
namespace fs = std::filesystem;
namespace {
struct Mod { Info info; fs::path dir; std::map<std::string,fs::path> files; std::set<std::string> texture_keys; };
fs::path root;
bool initialized=false, mounted=false, refreshed_after_mount=false;
std::vector<Mod> mods;
std::vector<std::string> enabled_ids, order, active_ids;
std::map<std::string,fs::path> replacements;
std::set<std::string> used;
std::vector<fs::path> texture_dirs;
std::string error;
std::string Key(const fs::path& p) {
    auto u=p.generic_u8string(); std::string s(u.begin(),u.end());
    std::transform(s.begin(),s.end(),s.begin(),[](unsigned char c){return char(std::tolower(c));});
    return s;
}
bool Has(const std::vector<std::string>& v,const std::string& s) { return std::find(v.begin(),v.end(),s)!=v.end(); }
bool Inside(const fs::path& p,const fs::path& base) {
    // Resolve junctions/symlinks too. Component comparison prevents prefix tricks.
    const auto rel=fs::weakly_canonical(p).lexically_relative(fs::weakly_canonical(base));
    if(rel.empty() || rel.is_absolute()) return false;
    for(const auto& c:rel) if(c=="..") return false;
    return true;
}
bool SafeId(const std::string& s) {
    if(s.empty() || s.size()>64) return false;
    for(unsigned char c:s) if(!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_')) return false;
    return true;
}
std::vector<std::string> Selected() {
    std::vector<std::string> r;
    for(const auto& m:mods) if(m.info.enabled && m.info.error.empty()) r.push_back(m.info.id);
    return r;
}
bool Save() {
    try {
        fs::create_directories(root);
        toml::array e,o; for(const auto& id:enabled_ids)e.push_back(id); for(const auto& m:mods)o.push_back(m.info.id);
        toml::table t{{"schema",1},{"enabled",std::move(e)},{"order",std::move(o)}};
        auto tmp=root/"state.toml.tmp";
        { std::ofstream f(tmp,std::ios::binary|std::ios::trunc); f<<t; f.flush(); if(!f)throw std::runtime_error("Cannot write mod selection"); }
#ifdef _WIN32
        if(!MoveFileExW(tmp.c_str(),(root/"state.toml").c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("Cannot save mod selection");
#else
        fs::rename(tmp,root/"state.toml");
#endif
        error.clear(); return true;
    } catch(const std::exception& e) { error=e.what(); return false; }
}
void Conflicts() {
    std::set<std::string> seen;
    for(auto& m:mods) {
        m.info.conflicts=0;
        if(!m.info.enabled || !m.info.error.empty())continue;
        for(const auto& f:m.files) if(!seen.insert("file:"+f.first).second)++m.info.conflicts;
        for(const auto& k:m.texture_keys)if(!seen.insert("texture:"+k).second)++m.info.conflicts;
    }
}
}
void Initialize(const fs::path& supplied) {
    if(initialized)return;
    initialized=true;
    if(!supplied.empty())root=supplied;
    else if(const char* env=std::getenv("NWII_MODS_DIR"))root=fs::u8path(env);
    else {
#ifdef _WIN32
        wchar_t exe[32768]{}; GetModuleFileNameW(nullptr,exe,32768); auto dir=fs::path(exe).parent_path();
#else
        auto dir=fs::canonical("/proc/self/exe").parent_path();
#endif
        root=(Key(dir.filename())=="bin" ? dir.parent_path() : dir)/"mods";
    }
    root=fs::absolute(root);
    Rescan();
}
fs::path Root(){Initialize();return root;}
std::vector<Info> List(){Initialize();std::vector<Info> r;for(const auto& m:mods)r.push_back(m.info);return r;}
std::string Error(){Initialize();return error;}
bool Rescan() {
    if(!initialized){Initialize();return error.empty();}
    if(mounted)refreshed_after_mount=true;
    mods.clear();enabled_ids.clear();order.clear();error.clear();
    try {
        if(fs::exists(root/"state.toml")) {
            if(fs::file_size(root/"state.toml")>1048576)throw std::runtime_error("Mod state too large");
            auto t=toml::parse_file((root/"state.toml").string());
            if(t["schema"].value_or(0)!=1)throw std::runtime_error("Unsupported mod state schema");
            for(const auto& name:{"enabled","order"}) {
                if(auto a=t[name].as_array())for(const auto& v:*a)if(auto s=v.value<std::string>()) (std::string(name)=="enabled"?enabled_ids:order).push_back(*s);
            }
        }
        if(!fs::exists(root))return true;
        std::vector<fs::path> dirs;
        for(const auto& d:fs::directory_iterator(root))if(d.is_directory() && fs::exists(d.path()/"mod.toml"))dirs.push_back(d.path());
        std::sort(dirs.begin(),dirs.end()); std::set<std::string> ids;
        for(const auto& dir:dirs) {
            Mod m;m.dir=dir;m.info.name=dir.filename().string();
            try {
                if(!Inside(dir,root) || !Inside(dir/"mod.toml",dir))throw std::runtime_error("Mod path leaves its folder");
                if(fs::file_size(dir/"mod.toml")>65536)throw std::runtime_error("Manifest too large");
                auto t=toml::parse_file((dir/"mod.toml").string());
                m.info.id=t["id"].value_or(std::string());
                if(t["schema"].value_or(0)!=1 || !SafeId(m.info.id))throw std::runtime_error("Expected schema=1 and a lowercase id");
                if(!ids.insert(m.info.id).second)throw std::runtime_error("Duplicate mod id");
                if(t["game"].value_or(std::string())!="SOUE01")throw std::runtime_error("Mod is not for SOUE01 (USA Wii 1.0)");
                m.info.name=t["name"].value_or(m.info.id);m.info.author=t["author"].value_or(std::string());
                m.info.version=t["version"].value_or(std::string());m.info.description=t["description"].value_or(std::string());
                for(const auto& folder:{"files","textures"}) {
                    const auto base=dir/folder;
                    if(!fs::exists(base))continue;
                    if(!fs::is_directory(base)||!Inside(base,dir))throw std::runtime_error("Asset folder leaves mod directory");
                    size_t count=0;
                    for(const auto& f:fs::recursive_directory_iterator(base)) {
                        if(!Inside(f.path(),base))throw std::runtime_error("Asset path leaves its folder");
                        if(!f.is_regular_file())continue;
                        if(++count>100000)throw std::runtime_error("Too many assets");
                        if(std::string(folder)=="files") {
                            const auto rel=f.path().lexically_relative(base); const auto key=Key(rel);
                            if(!fs::file_size(f.path())||fs::file_size(f.path())>0x7fffffffu)throw std::runtime_error("Empty or oversized game asset");
                            // Executable RELs / core system files are not supported by this asset loader.
                            if(Key(rel.extension())==".rel" || Key(rel.extension())==".dol" || key.rfind("sys/",0)==0)throw std::runtime_error("Executable/system replacements are unsupported");
                            if(!m.files.emplace(key,f.path()).second)throw std::runtime_error("Duplicate asset path");
                        } else {
                            const auto parsed=gx::hdtex::ParsePackFileName(Key(f.path().filename()));
                            if(!parsed.ok)continue;
                            ++m.info.textures;
                            if(!parsed.mip) {
                                auto key=parsed.key;
                                if(const auto p=key.find("_m_");p!=std::string::npos)key.erase(p,2);
                                m.texture_keys.insert(key);
                            }
                        }
                    }
                }
                m.info.files=m.files.size();m.info.enabled=Has(enabled_ids,m.info.id);
                if(!m.info.files&&m.texture_keys.empty())throw std::runtime_error("No game files or base tex1_ PNG/DDS textures found");
            } catch(const std::exception& e){m.info.error=e.what();m.info.enabled=false;}
            mods.push_back(std::move(m));
        }
        auto rank=[](const Mod& m){auto it=std::find(order.begin(),order.end(),m.info.id);return it==order.end()?order.size():size_t(it-order.begin());};
        std::stable_sort(mods.begin(),mods.end(),[&](const Mod& a,const Mod& b){return rank(a)<rank(b);});
        Conflicts();return true;
    }catch(const std::exception& e){error=e.what();mods.clear();return false;}
}
bool Enable(const std::string& id,bool value) {
    Initialize();auto old=enabled_ids;
    for(auto& m:mods)if(m.info.id==id && (!value || m.info.error.empty())) {
        enabled_ids.erase(std::remove(enabled_ids.begin(),enabled_ids.end(),id),enabled_ids.end());if(value)enabled_ids.push_back(id);
        if(!Save()){enabled_ids=old;return false;}m.info.enabled=value;Conflicts();return true;
    }return false;
}
bool Move(const std::string& id,int direction) {
    Initialize();for(size_t i=0;i<mods.size();++i)if(mods[i].info.id==id) {
        const int j=int(i)+(direction<0?-1:1);if(j<0||j>=int(mods.size()))return false;
        std::swap(mods[i],mods[j]);if(!Save()){std::swap(mods[i],mods[j]);return false;}Conflicts();return true;
    }return false;
}
bool RestartRequired(){Initialize();return mounted && (refreshed_after_mount || Selected()!=active_ids);}
void Mount() {
    Initialize();if(mounted)return;mounted=true;
    const char* flag=std::getenv("NWII_MODS");if(flag && std::string(flag)=="0"){std::cout<<"[Mods] Disabled by environment\n";return;}
    active_ids=Selected();
    for(const auto& m:mods)if(m.info.enabled && m.info.error.empty()) {
        for(const auto& f:m.files)replacements.emplace(f.first,f.second); // Top of list wins.
        if(m.info.textures)texture_dirs.push_back(m.dir/"textures");
        std::cout<<"[Mods] Mounted "<<m.info.id<<" files="<<m.info.files<<" textures="<<m.info.textures<<" conflicts="<<m.info.conflicts<<"\n";
    }
    if(!error.empty())std::cout<<"[Mods] ERROR: "<<error<<"\n";
}
fs::path Resolve(const fs::path& rel){Mount();auto it=replacements.find(Key(rel));if(it==replacements.end())return {};used.insert(it->first);return it->second;}
std::vector<fs::path> TextureDirs(){Mount();return texture_dirs;}
void ReportUnusedFiles(){for(const auto& f:replacements)if(!used.count(f.first)) {std::cout<<"[Mods] Unknown disc asset ignored: "<<f.first<<"\n";for(auto& m:mods)if(m.files.count(f.first)&&m.info.enabled)m.info.error="Unknown disc path: "+f.first;}}
}
