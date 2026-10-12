// Host-backed Wii NAND. See include/runtime/nand_fs.h.
// Semantics follow Dolphin (dolphin-emu/dolphin master, 2026):
//   Source/Core/Core/IOS/FS/HostBackend/FS.cpp, File.cpp   (HostFileSystem)
//   Source/Core/Core/IOS/FS/FileSystemCommon.cpp           (path rules, CreateFullPath)
//   Source/Core/Common/NandPaths.cpp                       (host name escaping)
//   Source/Core/Core/IOS/ES/ES.cpp, Formats.cpp            (boot dirs, DIVerify, uid.sys)
//   Source/Core/Core/SysConf.cpp, Boot/Boot_BS2Emu.cpp,
//   Common/SettingsHandler.cpp                             (SYSCONF, setting.txt)
#include "runtime/nand_fs.h"
#include "runtime/config.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <system_error>
#include <unordered_map>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace stdfs = std::filesystem;

namespace nwii::runtime::nand {

namespace {

stdfs::path u8_to_path(const std::string& text) {
    return stdfs::path(std::u8string(text.begin(), text.end()));
}

std::string path_to_u8(const stdfs::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

bool is_printable(char c) { return c >= 0x20 && c <= 0x7e; }

struct SplitPath {
    std::string parent;
    std::string file_name;
};

// Dolphin SplitPathAndBasename.
SplitPath split_path_and_basename(std::string_view path) {
    const size_t last = path.rfind('/');
    return {std::string(path.substr(0, std::max<size_t>(1, last))),
            std::string(path.substr(last + 1))};
}

std::vector<std::string> split_components(std::string_view path) {
    std::vector<std::string> out;
    size_t begin = 1;
    while (begin <= path.size()) {
        size_t end = path.find('/', begin);
        if (end == std::string_view::npos) end = path.size();
        out.emplace_back(path.substr(begin, end - begin));
        begin = end + 1;
    }
    return out;
}

bool is_illegal_character(char c) {
    static constexpr char illegal[] = {'"', '*', '/', ':', '<', '>', '?', '\\', '|', '\x7f'};
    return static_cast<unsigned char>(c) <= 0x1F ||
           std::find(std::begin(illegal), std::end(illegal), c) != std::end(illegal);
}

std::string replace_all(std::string text, std::string_view from, std::string_view to) {
    size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
    return text;
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int seek_host(std::FILE* file, uint64_t offset) {
#ifdef _WIN32
    return _fseeki64(file, static_cast<long long>(offset), SEEK_SET);
#else
    return fseeko(file, static_cast<off_t>(offset), SEEK_SET);
#endif
}

uint64_t host_size(std::FILE* file) {
    std::fflush(file);
#ifdef _WIN32
    _fseeki64(file, 0, SEEK_END);
    const long long size = _ftelli64(file);
#else
    fseeko(file, 0, SEEK_END);
    const long long size = ftello(file);
#endif
    return size < 0 ? 0 : static_cast<uint64_t>(size);
}

std::FILE* fopen_path(const stdfs::path& path, const char* mode) {
#ifdef _WIN32
    std::wstring wide_mode(mode, mode + std::strlen(mode));
    return _wfopen(path.c_str(), wide_mode.c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

void put_be16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
void put_be32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}
uint16_t get_be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
uint32_t get_be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
uint64_t get_be64(const uint8_t* p) { return (uint64_t(get_be32(p)) << 32) | get_be32(p + 4); }

std::string hex8(uint32_t v) {
    char buf[9];
    std::snprintf(buf, sizeof(buf), "%08x", v);
    return buf;
}

constexpr Modes RW_ALL{MODE_RW, MODE_RW, MODE_RW};

}  // namespace

// ---- Path rules (Dolphin FileSystemCommon.cpp) ------------------------------

bool is_valid_non_root_path(std::string_view path) {
    if (!(path.length() > 1 && path.length() <= MAX_PATH_LENGTH && path[0] == '/' &&
          path.back() != '/'))
        return false;
    // Stricter than Dolphin: an empty component ("//") has no NAND node name and
    // would alias its parent on the host.
    return path.find("//") == std::string_view::npos;
}

bool is_valid_path(std::string_view path) { return path == "/" || is_valid_non_root_path(path); }

bool is_valid_filename(std::string_view name) {
    return name.length() <= MAX_FILENAME_LENGTH && name.find('/') == std::string_view::npos;
}

std::string escape_file_name(const std::string& name) {
    if (std::all_of(name.begin(), name.end(), [](char c) { return c == '.'; }))
        return replace_all(name, ".", "__2e__");
    const std::string escaped_underscores = replace_all(name, "__", "__5f____5f__");
    std::string result;
    result.reserve(escaped_underscores.size());
    for (char c : escaped_underscores) {
        if (is_illegal_character(c)) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "__%02x__", static_cast<unsigned char>(c));
            result += buf;
        } else {
            result.push_back(c);
        }
    }
    return result;
}

std::string unescape_file_name(const std::string& name) {
    std::string result = name;
    size_t pos = 0;
    while ((pos = result.find("__", pos)) != std::string::npos) {
        if (pos + 6 <= result.size() && result[pos + 4] == '_' && result[pos + 5] == '_') {
            const int hi = hex_value(result[pos + 2]);
            const int lo = hex_value(result[pos + 3]);
            if (hi >= 0 && lo >= 0)
                result.replace(pos, 6, 1, static_cast<char>(hi * 16 + lo));
        }
        ++pos;
    }
    return result;
}

// ---- HostFS ------------------------------------------------------------------

bool HostFS::FstEntry::check_permission(uint32_t caller_uid, uint16_t caller_gid,
                                        uint8_t requested) const {
    if (caller_uid == 0) return true;
    uint8_t file_mode = data.modes.other;
    if (data.uid == caller_uid)
        file_mode = data.modes.owner;
    else if (data.gid == caller_gid)
        file_mode = data.modes.group;
    return (requested & file_mode) == requested;
}

HostFS::HostFS(stdfs::path root) : m_root(std::move(root)) {
    std::error_code ec;
    stdfs::create_directories(m_root, ec);
    reset_fst();
    load_fst();
}

HostFS::~HostFS() {
    std::lock_guard lock(m_mutex);
    for (auto& handle : m_handles) handle = Handle{};
}

stdfs::path HostFS::host_path(const std::string& wii_path) const {
    stdfs::path result = m_root;
    if (wii_path.size() <= 1) return result;
    for (const std::string& component : split_components(wii_path))
        result /= u8_to_path(escape_file_name(component));
    return result;
}

void HostFS::reset_fst() {
    m_root_entry = FstEntry{};
    m_root_entry.name = "/";
    // Mode 0x16 (Directory | Owner_None | Group_Read | Other_Read) in the FS sysmodule.
    m_root_entry.data.modes = {MODE_NONE, MODE_READ, MODE_READ};
}

// fst.bin: Dolphin SerializedFstEntry, 0x20 bytes each, preorder:
// name[12], uid BE32, gid BE16, is_file u8, modes[3], attribute u8, pad u8,
// x3 BE32, num_children BE32.
void HostFS::load_fst() {
    std::ifstream file(m_root / "fst.bin", std::ios::binary);
    if (!file) return;
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    size_t position = 0;
    bool ok = true;
    auto parse = [&](auto& self, size_t depth) -> FstEntry {
        FstEntry result;
        if (depth > MAX_PATH_DEPTH || position + 0x20 > bytes.size()) { ok = false; return result; }
        const uint8_t* e = bytes.data() + position;
        position += 0x20;
        result.name.assign(reinterpret_cast<const char*>(e), strnlen(reinterpret_cast<const char*>(e), 12));
        result.data.uid = get_be32(e + 12);
        result.data.gid = get_be16(e + 16);
        result.data.is_file = e[18] != 0;
        result.data.modes = {e[19], e[20], e[21]};
        result.data.attribute = e[22];
        const uint32_t children = get_be32(e + 28);
        for (uint32_t i = 0; i < children && ok; ++i)
            result.children.push_back(self(self, depth + 1));
        return result;
    };
    FstEntry root = parse(parse, 0);
    if (!ok) {
        std::cout << "[NAND] fst.bin is invalid; using default metadata" << std::endl;
        return;
    }
    m_root_entry = std::move(root);
}

void HostFS::save_fst() {
    std::vector<uint8_t> out;
    auto collect = [&](auto& self, const FstEntry& entry) -> void {
        const size_t base = out.size();
        out.resize(base + 0x20, 0);
        uint8_t* e = out.data() + base;
        std::memcpy(e, entry.name.data(), std::min<size_t>(12, entry.name.size()));
        put_be32(e + 12, entry.data.uid);
        put_be16(e + 16, entry.data.gid);
        e[18] = entry.data.is_file ? 1 : 0;
        e[19] = entry.data.modes.owner;
        e[20] = entry.data.modes.group;
        e[21] = entry.data.modes.other;
        e[22] = entry.data.attribute;
        put_be32(e + 28, static_cast<uint32_t>(entry.children.size()));
        for (const FstEntry& child : entry.children) self(self, child);
    };
    collect(collect, m_root_entry);
    const stdfs::path dest = m_root / "fst.bin";
    const stdfs::path temp = m_root / "fst.bin.tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
        if (!file) {
            std::cout << "[NAND] failed to write " << path_to_u8(temp) << std::endl;
            return;
        }
    }
    std::error_code ec;
    stdfs::rename(temp, dest, ec);
    if (ec) std::cout << "[NAND] failed to replace fst.bin: " << ec.message() << std::endl;
}

// Dolphin GetFstEntryForPath: existing host nodes without metadata get a
// default entry (uid 0, gid 0, modes rw/rw/rw).
HostFS::FstEntry* HostFS::get_entry(const std::string& path) {
    if (path == "/") return &m_root_entry;
    if (!is_valid_non_root_path(path)) return nullptr;
    const stdfs::path host = host_path(path);
    std::error_code ec;
    if (!stdfs::exists(host, ec)) return nullptr;
    FstEntry* entry = &m_root_entry;
    for (const std::string& component : split_components(path)) {
        auto next = std::find_if(entry->children.begin(), entry->children.end(),
                                 [&](const FstEntry& child) { return child.name == component; });
        if (next != entry->children.end()) {
            entry = &*next;
        } else {
            entry = &entry->children.emplace_back();
            entry->name = component;
            entry->data.modes = RW_ALL;
        }
    }
    entry->data.is_file = stdfs::is_regular_file(host, ec);
    if (entry->data.is_file) entry->children.clear();
    return entry;
}

int32_t HostFS::format(uint32_t uid) {
    std::lock_guard lock(m_mutex);
    if (uid != 0) return FS_EACCESS;
    for (auto& handle : m_handles) handle = Handle{};
    std::error_code ec;
    stdfs::remove_all(m_root, ec);
    stdfs::create_directories(m_root, ec);
    if (ec) return FS_EUNKNOWN;
    reset_fst();
    save_fst();
    return FS_SUCCESS;
}

int32_t HostFS::create_node(uint32_t uid, uint16_t gid, const std::string& path, uint8_t attr,
                            Modes modes, bool is_file) {
    std::lock_guard lock(m_mutex);
    if (!is_valid_non_root_path(path) || !std::all_of(path.begin(), path.end(), is_printable))
        return FS_EINVAL;
    if (!is_file && static_cast<size_t>(std::count(path.begin(), path.end(), '/')) > MAX_PATH_DEPTH)
        return FS_ETOODEEP;
    const SplitPath split = split_path_and_basename(path);
    const stdfs::path host = host_path(path);
    FstEntry* parent = get_entry(split.parent);
    if (!parent) return FS_ENOENT;
    if (!parent->check_permission(uid, gid, MODE_WRITE)) return FS_EACCESS;
    std::error_code ec;
    if (stdfs::exists(host, ec)) return FS_EEXIST;
    bool ok;
    if (is_file) {
        std::FILE* file = fopen_path(host, "wb");
        ok = file != nullptr;
        if (file) std::fclose(file);
    } else {
        ok = stdfs::create_directory(host, ec) && !ec;
    }
    if (!ok) {
        std::cout << "[NAND] failed to create " << path_to_u8(host) << std::endl;
        return FS_EUNKNOWN;
    }
    FstEntry* child = get_entry(path);
    *child = FstEntry{};
    child->name = split.file_name;
    child->data.is_file = is_file;
    child->data.modes = modes;
    child->data.uid = uid;
    child->data.gid = gid;
    child->data.attribute = attr;
    save_fst();
    return FS_SUCCESS;
}

int32_t HostFS::create_file(uint32_t uid, uint16_t gid, const std::string& path, uint8_t attr, Modes modes) {
    return create_node(uid, gid, path, attr, modes, true);
}

int32_t HostFS::create_directory(uint32_t uid, uint16_t gid, const std::string& path, uint8_t attr, Modes modes) {
    return create_node(uid, gid, path, attr, modes, false);
}

int32_t HostFS::create_full_path(uint32_t uid, uint16_t gid, const std::string& path, uint8_t attr, Modes modes) {
    std::lock_guard lock(m_mutex);
    std::string::size_type position = 1;
    while (true) {
        position = path.find('/', position);
        if (position == std::string::npos) return FS_SUCCESS;
        const std::string subpath = path.substr(0, position);
        Metadata metadata;
        const int32_t result = get_metadata(uid, gid, subpath, metadata);
        if (result != FS_SUCCESS && result != FS_ENOENT) return result;
        if (result == FS_SUCCESS && metadata.is_file) return FS_EINVAL;
        if (result == FS_ENOENT) {
            const int32_t created = create_directory(uid, gid, subpath, attr, modes);
            if (created != FS_SUCCESS) return created;
        }
        ++position;
    }
}

bool HostFS::is_file_opened(const std::string& path) const {
    return std::any_of(m_handles.begin(), m_handles.end(),
                       [&](const Handle& h) { return h.opened && h.wii_path == path; });
}

bool HostFS::is_directory_in_use(const std::string& path) const {
    // Dolphin uses a plain prefix test; require a component boundary here.
    return std::any_of(m_handles.begin(), m_handles.end(), [&](const Handle& h) {
        return h.opened && h.wii_path.size() > path.size() && h.wii_path.starts_with(path) &&
               h.wii_path[path.size()] == '/';
    });
}

int32_t HostFS::remove(uint32_t uid, uint16_t gid, const std::string& path) {
    std::lock_guard lock(m_mutex);
    if (!is_valid_non_root_path(path)) return FS_EINVAL;
    const stdfs::path host = host_path(path);
    const SplitPath split = split_path_and_basename(path);
    FstEntry* parent = get_entry(split.parent);
    if (!parent) return FS_ENOENT;
    if (!parent->check_permission(uid, gid, MODE_WRITE)) return FS_EACCESS;
    std::error_code ec;
    if (!stdfs::exists(host, ec)) return FS_ENOENT;
    if (stdfs::is_regular_file(host, ec) && !is_file_opened(path))
        stdfs::remove(host, ec);
    else if (stdfs::is_directory(host, ec) && !is_directory_in_use(path))
        stdfs::remove_all(host, ec);
    else
        return FS_EINUSE;
    if (ec) std::cout << "[NAND] host delete failed for " << path << ": " << ec.message() << std::endl;
    auto it = std::find_if(parent->children.begin(), parent->children.end(),
                           [&](const FstEntry& child) { return child.name == split.file_name; });
    if (it != parent->children.end()) parent->children.erase(it);
    save_fst();
    return FS_SUCCESS;
}

int32_t HostFS::rename(uint32_t uid, uint16_t gid, const std::string& old_path, const std::string& new_path) {
    std::lock_guard lock(m_mutex);
    if (!is_valid_non_root_path(old_path) || !is_valid_non_root_path(new_path)) return FS_EINVAL;
    const SplitPath split_old = split_path_and_basename(old_path);
    const SplitPath split_new = split_path_and_basename(new_path);
    FstEntry* old_parent = get_entry(split_old.parent);
    FstEntry* new_parent = get_entry(split_new.parent);
    if (!old_parent || !new_parent) return FS_ENOENT;
    if (!old_parent->check_permission(uid, gid, MODE_WRITE) ||
        !new_parent->check_permission(uid, gid, MODE_WRITE))
        return FS_EACCESS;
    FstEntry* entry = get_entry(old_path);
    if (!entry) return FS_ENOENT;
    if (entry->data.is_file && split_old.file_name != split_new.file_name) return FS_EINVAL;
    if ((!entry->data.is_file && is_directory_in_use(old_path)) ||
        (entry->data.is_file && is_file_opened(old_path)))
        return FS_EINUSE;
    // Not in Dolphin: renaming a node onto itself would delete it below.
    if (old_path == new_path) return FS_SUCCESS;
    const stdfs::path host_old = host_path(old_path);
    const stdfs::path host_new = host_path(new_path);
    std::error_code ec;
    if (stdfs::exists(host_new, ec)) {
        const bool old_is_file = stdfs::is_regular_file(host_old, ec);
        const bool new_is_file = stdfs::is_regular_file(host_new, ec);
        if (old_is_file && new_is_file)
            stdfs::remove(host_new, ec);
        else if (!old_is_file && !new_is_file)
            stdfs::remove_all(host_new, ec);
        else
            return FS_EINVAL;
    }
    ec.clear();
    stdfs::rename(host_old, host_new, ec);
    if (ec) {
        std::cout << "[NAND] rename " << old_path << " -> " << new_path << " failed: " << ec.message() << std::endl;
        return FS_ENOENT;
    }
    FstEntry* new_entry = get_entry(new_path);
    new_entry->name = split_new.file_name;
    auto it = std::find_if(old_parent->children.begin(), old_parent->children.end(),
                           [&](const FstEntry& child) { return child.name == split_old.file_name; });
    if (it != old_parent->children.end()) {
        new_entry->data = it->data;
        new_entry->children = it->children;
        old_parent->children.erase(it);
    }
    save_fst();
    return FS_SUCCESS;
}

int32_t HostFS::read_directory(uint32_t uid, uint16_t gid, const std::string& path,
                               std::vector<std::string>& out) {
    std::lock_guard lock(m_mutex);
    out.clear();
    if (!is_valid_path(path)) return FS_EINVAL;
    const FstEntry* entry = get_entry(path);
    if (!entry) return FS_ENOENT;
    if (!entry->check_permission(uid, gid, MODE_READ)) return FS_EACCESS;
    if (entry->data.is_file) return FS_EINVAL;
    const bool is_root = path == "/";
    std::error_code ec;
    std::vector<std::string> names;
    for (const auto& child : stdfs::directory_iterator(host_path(path), ec)) {
        std::error_code type_ec;
        const bool is_dir = child.is_directory(type_ec);
        // Dolphin FixupDirectoryEntries: root files are host bookkeeping (fst.bin).
        if (is_root && !is_dir) continue;
        std::string name = path_to_u8(child.path().filename());
        if (name.find("__") != std::string::npos) name = unescape_file_name(name);
        if (!is_valid_filename(name)) continue;
        names.push_back(std::move(name));
    }
    // Dolphin order: FST (creation) order descending, unknown names last, then name descending.
    std::unordered_map<std::string, int> sort_keys;
    int index = 0;
    for (const FstEntry& child : entry->children) sort_keys.emplace(child.name, index++);
    auto key = [&](const std::string& name) {
        auto it = sort_keys.find(name);
        return it != sort_keys.end() ? it->second : -1;
    };
    std::sort(names.begin(), names.end(), [&](const std::string& a, const std::string& b) {
        const int ka = key(a), kb = key(b);
        if (ka != kb) return ka > kb;
        return a > b;
    });
    out = std::move(names);
    return FS_SUCCESS;
}

int32_t HostFS::get_metadata(uint32_t uid, uint16_t gid, const std::string& path, Metadata& out) {
    std::lock_guard lock(m_mutex);
    const FstEntry* entry = nullptr;
    if (path == "/") {
        entry = &m_root_entry;
    } else {
        if (!is_valid_non_root_path(path)) return FS_EINVAL;
        const SplitPath split = split_path_and_basename(path);
        const FstEntry* parent = get_entry(split.parent);
        if (!parent) return FS_ENOENT;
        if (!parent->check_permission(uid, gid, MODE_READ)) return FS_EACCESS;
        entry = get_entry(path);
    }
    if (!entry) return FS_ENOENT;
    out = entry->data;
    std::error_code ec;
    const stdfs::path host = host_path(path);
    out.size = stdfs::is_regular_file(host, ec) ? static_cast<uint32_t>(stdfs::file_size(host, ec)) : 0;
    return FS_SUCCESS;
}

int32_t HostFS::set_metadata(uint32_t caller_uid, const std::string& path, uint32_t uid, uint16_t gid,
                             uint8_t attr, Modes modes) {
    std::lock_guard lock(m_mutex);
    if (!is_valid_path(path)) return FS_EINVAL;
    FstEntry* entry = get_entry(path);
    if (!entry) return FS_ENOENT;
    if (caller_uid != 0 && caller_uid != entry->data.uid) return FS_EACCESS;
    if (caller_uid != 0 && uid != entry->data.uid) return FS_EACCESS;
    std::error_code ec;
    const stdfs::path host = host_path(path);
    const bool is_empty = !stdfs::is_regular_file(host, ec) || stdfs::file_size(host, ec) == 0;
    if (entry->data.uid != uid && entry->data.is_file && !is_empty) return FS_ENOTEMPTY;
    if (entry->data.gid != gid || entry->data.uid != uid || entry->data.attribute != attr ||
        !(entry->data.modes == modes)) {
        entry->data.gid = gid;
        entry->data.uid = uid;
        entry->data.attribute = attr;
        entry->data.modes = modes;
        save_fst();
    }
    return FS_SUCCESS;
}

int32_t HostFS::get_directory_stats(const std::string& path, DirectoryStats& out) {
    std::lock_guard lock(m_mutex);
    out = {};
    if (!is_valid_path(path)) return FS_EINVAL;
    const stdfs::path host = host_path(path);
    std::error_code ec;
    if (!stdfs::exists(host, ec)) return FS_ENOENT;
    if (!stdfs::is_directory(host, ec)) return FS_EINVAL;
    uint64_t entries = 0, clusters = 0;
    auto scan = [&](auto& self, const stdfs::path& dir, bool is_root) -> void {
        std::error_code it_ec;
        for (const auto& child : stdfs::directory_iterator(dir, it_ec)) {
            std::error_code type_ec;
            const bool is_dir = child.is_directory(type_ec);
            if (is_root && !is_dir) continue;
            std::string name = path_to_u8(child.path().filename());
            if (name.find("__") != std::string::npos) name = unescape_file_name(name);
            if (!is_valid_filename(name)) continue;
            ++entries;
            if (is_dir) {
                self(self, child.path(), false);
            } else {
                const uint64_t size = child.file_size(type_ec);
                clusters += (size + CLUSTER_SIZE - 1) / CLUSTER_SIZE;
            }
        }
    };
    scan(scan, host, path == "/");
    out.used_inodes = static_cast<uint32_t>(std::min<uint64_t>(1 + entries, TOTAL_INODES));
    out.used_clusters = static_cast<uint32_t>(std::min<uint64_t>(clusters, USABLE_CLUSTERS));
    return FS_SUCCESS;
}

int32_t HostFS::get_nand_stats(NandStats& out) {
    DirectoryStats root{};
    const int32_t result = get_directory_stats("/", root);
    if (result != FS_SUCCESS) return result;
    out.cluster_size = CLUSTER_SIZE;
    out.free_clusters = USABLE_CLUSTERS - root.used_clusters;
    out.used_clusters = root.used_clusters;
    out.bad_clusters = 0;
    out.reserved_clusters = RESERVED_CLUSTERS;
    out.free_inodes = TOTAL_INODES - root.used_inodes;
    out.used_inodes = root.used_inodes;
    return FS_SUCCESS;
}

std::shared_ptr<std::FILE> HostFS::open_host_file(const stdfs::path& path) {
    // Dolphin shares one host file per path across handles (HostBackend/File.cpp).
    if (auto it = m_open_files.find(path); it != m_open_files.end())
        if (auto existing = it->second.lock()) return existing;
    std::FILE* file = fopen_path(path, "r+b");
    if (!file) file = fopen_path(path, "rb");
    if (!file) return nullptr;
    std::shared_ptr<std::FILE> shared(file, [this, path](std::FILE* f) {
        std::fclose(f);
        m_open_files.erase(path);
    });
    m_open_files[path] = shared;
    return shared;
}

HostFS::Handle* HostFS::get_handle(int32_t fd) {
    if (fd < 0 || fd >= MAX_HANDLES || !m_handles[fd].opened) return nullptr;
    return &m_handles[fd];
}

int HostFS::open_handle_count() const {
    std::lock_guard lock(m_mutex);
    return static_cast<int>(std::count_if(m_handles.begin(), m_handles.end(),
                                          [](const Handle& h) { return h.opened; }));
}

int32_t HostFS::open_file(uint32_t, uint16_t, const std::string& path, uint8_t mode) {
    std::lock_guard lock(m_mutex);
    auto slot = std::find_if(m_handles.begin(), m_handles.end(), [](const Handle& h) { return !h.opened; });
    if (slot == m_handles.end()) return FS_ENOHANDLE;
    if (!path.starts_with("/") || path.find("//") != std::string::npos) return FS_ENOENT;
    const stdfs::path host = host_path(path);
    std::error_code ec;
    if (stdfs::is_directory(host, ec)) return FS_EINVAL;
    if (!stdfs::is_regular_file(host, ec)) return FS_ENOENT;
    Handle handle;
    handle.file = open_host_file(host);
    if (!handle.file) return FS_EACCESS;
    handle.opened = true;
    handle.wii_path = path;
    handle.mode = mode;
    handle.offset = 0;
    *slot = std::move(handle);
    return static_cast<int32_t>(slot - m_handles.begin());
}

int32_t HostFS::close(int32_t fd) {
    std::lock_guard lock(m_mutex);
    Handle* handle = get_handle(fd);
    if (!handle) return FS_EINVAL;
    *handle = Handle{};
    return FS_SUCCESS;
}

int32_t HostFS::read(int32_t fd, uint8_t* dst, uint32_t count) {
    std::lock_guard lock(m_mutex);
    Handle* handle = get_handle(fd);
    if (!handle || !handle->file) return FS_EINVAL;
    if ((handle->mode & MODE_READ) == 0) return FS_EACCESS;
    const uint32_t file_size = static_cast<uint32_t>(host_size(handle->file.get()));
    // IOS clamps the request to the bytes left (Dolphin ReadBytesFromFile).
    if (handle->offset >= file_size)
        count = 0;
    else if (uint64_t(count) + handle->offset > file_size)
        count = file_size - handle->offset;
    if (count == 0) return 0;
    seek_host(handle->file.get(), handle->offset);
    const uint32_t actually_read = static_cast<uint32_t>(std::fread(dst, 1, count, handle->file.get()));
    if (actually_read != count && std::ferror(handle->file.get())) {
        std::clearerr(handle->file.get());
        return FS_EACCESS;
    }
    handle->offset += actually_read;
    return static_cast<int32_t>(actually_read);
}

int32_t HostFS::write(int32_t fd, const uint8_t* src, uint32_t count) {
    std::lock_guard lock(m_mutex);
    Handle* handle = get_handle(fd);
    if (!handle || !handle->file) return FS_EINVAL;
    if ((handle->mode & MODE_WRITE) == 0) return FS_EACCESS;
    seek_host(handle->file.get(), handle->offset);
    if (count && std::fwrite(src, 1, count, handle->file.get()) != count) {
        std::clearerr(handle->file.get());
        return FS_EACCESS;
    }
    // Write-through: the NAND must survive a crash or a killed process.
    std::fflush(handle->file.get());
    handle->offset += count;
    return static_cast<int32_t>(count);
}

int32_t HostFS::seek(int32_t fd, uint32_t offset, uint32_t whence) {
    std::lock_guard lock(m_mutex);
    Handle* handle = get_handle(fd);
    if (!handle || !handle->file) return FS_EINVAL;
    const uint32_t size = static_cast<uint32_t>(host_size(handle->file.get()));
    uint32_t position;
    switch (whence) {
    case 0: position = offset; break;
    case 1: position = handle->offset + offset; break;
    case 2: position = size + offset; break;
    default: return FS_EINVAL;
    }
    // Unlike POSIX, IOS refuses to seek past the end of the file.
    if (size < position) return FS_EINVAL;
    handle->offset = position;
    return static_cast<int32_t>(position);
}

int32_t HostFS::file_status(int32_t fd, uint32_t& size, uint32_t& offset) {
    std::lock_guard lock(m_mutex);
    Handle* handle = get_handle(fd);
    if (!handle || !handle->file) return FS_EINVAL;
    size = static_cast<uint32_t>(host_size(handle->file.get()));
    offset = handle->offset;
    return FS_SUCCESS;
}

bool HostFS::read_whole_file(const std::string& path, std::vector<uint8_t>& out) {
    std::lock_guard lock(m_mutex);
    out.clear();
    if (!is_valid_non_root_path(path)) return false;
    std::error_code ec;
    const stdfs::path host = host_path(path);
    if (!stdfs::is_regular_file(host, ec)) return false;
    std::ifstream file(host, std::ios::binary);
    if (!file) return false;
    out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}

int32_t HostFS::write_file_atomic(uint32_t uid, uint16_t gid, const std::string& path,
                                  const std::vector<uint8_t>& data, Modes modes) {
    std::lock_guard lock(m_mutex);
    const SplitPath split = split_path_and_basename(path);
    const std::string temp = "/tmp/" + split.file_name;
    remove(PID_KERNEL, PID_KERNEL, temp);
    int32_t result = create_file(uid, gid, temp, 0, modes);
    if (result != FS_SUCCESS) return result;
    const int32_t fd = open_file(uid, gid, temp, MODE_RW);
    if (fd < 0) return fd;
    const int32_t written = write(fd, data.data(), static_cast<uint32_t>(data.size()));
    close(fd);
    if (written != static_cast<int32_t>(data.size())) return written < 0 ? written : FS_EUNKNOWN;
    return rename(uid, gid, temp, path);
}

// ---- Runtime wiring ------------------------------------------------------------

namespace {

std::mutex g_instance_mutex;
std::unique_ptr<HostFS> g_instance;
uint32_t g_ppc_uid = 0x1001;
uint16_t g_ppc_gid = 0x3031;

stdfs::path exe_dir() {
#ifdef _WIN32
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length > 0 && length < buffer.size()) {
        buffer.resize(length);
        return stdfs::path(buffer).parent_path();
    }
#else
    std::error_code ec;
    const stdfs::path self = stdfs::read_symlink("/proc/self/exe", ec);
    if (!ec) return self.parent_path();
#endif
    return stdfs::current_path();
}

}  // namespace

stdfs::path resolve_root(bool* hermetic) {
    if (hermetic) *hermetic = false;
    if (const char* env = std::getenv("NWII_NAND_ROOT"); env && *env) return u8_to_path(env);
    if (!Config::get().nand_root.empty()) return u8_to_path(Config::get().nand_root);
    const stdfs::path base = exe_dir().parent_path();
    if (const char* replay = std::getenv("NWII_SS_INPUT_REPLAY"); replay && *replay) {
        if (hermetic) *hermetic = true;
        return base / "nand-replay";
    }
    return base / "nand";
}

HostFS& instance() {
    std::lock_guard lock(g_instance_mutex);
    if (!g_instance) {
        bool hermetic = false;
        const stdfs::path root = resolve_root(&hermetic);
        if (hermetic && root.filename() == "nand-replay") {
            std::error_code ec;
            stdfs::remove_all(root, ec);
        }
        g_instance = std::make_unique<HostFS>(root);
        std::cout << "[NAND] root=" << path_to_u8(stdfs::absolute(root))
                  << (hermetic ? " (input replay: fresh NAND, wiped at boot)" : " (persistent)")
                  << std::endl;
    }
    return *g_instance;
}

void reset_instance(const stdfs::path& root, bool wipe) {
    std::lock_guard lock(g_instance_mutex);
    g_instance.reset();
    if (wipe) {
        std::error_code ec;
        stdfs::remove_all(root, ec);
    }
    g_instance = std::make_unique<HostFS>(root);
}

uint32_t ppc_uid() { return g_ppc_uid; }
uint16_t ppc_gid() { return g_ppc_gid; }

void boot_fs(HostFS& fs) {
    // Dolphin FSCore ctor: /tmp is reset whenever IOS (re)starts.
    if (fs.remove(PID_KERNEL, PID_KERNEL, "/tmp") == FS_SUCCESS)
        fs.create_directory(PID_KERNEL, PID_KERNEL, "/tmp", 0, RW_ALL);
}

namespace {

std::string title_path(uint64_t title_id) {
    return "/title/" + hex8(uint32_t(title_id >> 32)) + "/" + hex8(uint32_t(title_id));
}

// Dolphin ES::UIDSys::GetOrInsertUIDForTitle (/sys/uid.sys: BE64 title, BE32 uid).
uint32_t get_or_insert_uid(HostFS& fs, uint64_t title_id) {
    std::vector<uint8_t> bytes;
    fs.read_whole_file("/sys/uid.sys", bytes);
    std::vector<std::pair<uint64_t, uint32_t>> entries;
    for (size_t i = 0; i + 12 <= bytes.size(); i += 12) {
        const uint64_t tid = get_be64(bytes.data() + i);
        const uint32_t uid = get_be32(bytes.data() + i + 8);
        if (!tid && !uid) break;
        entries.emplace_back(tid, uid);
    }
    bool changed = false;
    auto lookup = [&](uint64_t tid) -> uint32_t {
        for (const auto& entry : entries)
            if (entry.first == tid) return entry.second;
        uint32_t next = 0x1000;  // FIRST_PPC_UID
        for (const auto& entry : entries) next = std::max(next, entry.second + 1);
        entries.emplace_back(tid, next);
        changed = true;
        return next;
    };
    if (entries.empty()) lookup(SYSTEM_MENU_TITLE);
    const uint32_t uid = lookup(title_id);
    if (changed) {
        std::vector<uint8_t> out(entries.size() * 12);
        for (size_t i = 0; i < entries.size(); ++i) {
            put_be32(out.data() + i * 12, uint32_t(entries[i].first >> 32));
            put_be32(out.data() + i * 12 + 4, uint32_t(entries[i].first));
            put_be32(out.data() + i * 12 + 8, entries[i].second);
        }
        fs.write_file_atomic(PID_KERNEL, PID_KERNEL, "/sys/uid.sys", out, {MODE_RW, MODE_RW, MODE_NONE});
    }
    return uid;
}

// Dolphin ESCore::CreateTitleDirectories / DIVerify.
void create_title_directories(HostFS& fs, uint64_t title_id, uint32_t uid, uint16_t gid,
                              const std::vector<uint8_t>* tmd) {
    const std::string base = title_path(title_id);
    const std::string content_dir = base + "/content";
    const std::string tmd_path = content_dir + "/title.tmd";
    fs.create_full_path(PID_KERNEL, PID_KERNEL, tmd_path, 0, {MODE_RW, MODE_RW, MODE_READ});
    fs.set_metadata(PID_KERNEL, content_dir, PID_KERNEL, PID_KERNEL, 0, {MODE_RW, MODE_RW, MODE_NONE});
    Metadata existing;
    if (tmd && !tmd->empty() && fs.get_metadata(PID_KERNEL, PID_KERNEL, tmd_path, existing) == FS_ENOENT)
        fs.write_file_atomic(PID_KERNEL, PID_KERNEL, tmd_path, *tmd, {MODE_RW, MODE_RW, MODE_NONE});
    const std::string data_dir = base + "/data";
    constexpr Modes data_dir_modes{MODE_RW, MODE_NONE, MODE_NONE};
    fs.create_directory(PID_KERNEL, PID_KERNEL, data_dir, 0, data_dir_modes);
    fs.set_metadata(PID_KERNEL, data_dir, uid, gid, 0, data_dir_modes);
}

}  // namespace

void boot_es(HostFS& fs, uint64_t title_id, uint16_t group_id, const stdfs::path& tmd_file) {
    struct DirectoryToCreate {
        const char* path;
        Modes modes;
        uint32_t uid = PID_KERNEL;
        uint16_t gid = PID_KERNEL;
    };
    static const DirectoryToCreate directories[] = {
        {"/sys", {MODE_RW, MODE_RW, MODE_NONE}},
        {"/ticket", {MODE_RW, MODE_RW, MODE_NONE}},
        {"/title", {MODE_RW, MODE_RW, MODE_READ}},
        {"/shared1", {MODE_RW, MODE_RW, MODE_NONE}},
        {"/shared2", RW_ALL},
        {"/tmp", RW_ALL},
        {"/import", {MODE_RW, MODE_RW, MODE_NONE}},
        {"/meta", RW_ALL, SYSMENU_UID, SYSMENU_GID},
    };
    for (const auto& directory : directories) {
        fs.create_directory(PID_KERNEL, PID_KERNEL, directory.path, 0, directory.modes);
        fs.set_metadata(PID_KERNEL, directory.path, directory.uid, directory.gid, 0, directory.modes);
    }
    const uint32_t uid = get_or_insert_uid(fs, title_id);
    g_ppc_uid = uid;
    g_ppc_gid = group_id;
    std::vector<uint8_t> tmd;
    std::error_code ec;
    if (!tmd_file.empty() && stdfs::is_regular_file(tmd_file, ec)) {
        std::ifstream file(tmd_file, std::ios::binary);
        tmd.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
    create_title_directories(fs, title_id, uid, group_id, &tmd);
    std::cout << "[NAND] title " << hex8(uint32_t(title_id >> 32)) << "-" << hex8(uint32_t(title_id))
              << " uid=0x" << std::hex << uid << " gid=0x" << group_id << std::dec
              << " data=" << title_path(title_id) << "/data" << std::endl;
}

// ---- SYSCONF (Dolphin SysConf.cpp) ------------------------------------------------

namespace {

enum SysConfType : uint8_t { BIG_ARRAY = 1, SMALL_ARRAY = 2, BYTE = 3, SHORT = 4, LONG = 5, LONG_LONG = 6, BYTE_BOOL = 7 };

struct SysConfEntry {
    SysConfType type;
    std::string name;
    std::vector<uint8_t> bytes;
};

std::vector<uint8_t> save_sysconf(const std::vector<SysConfEntry>& items) {
    constexpr size_t SYSCONF_SIZE = 0x4000;
    std::vector<uint8_t> buffer = {'S', 'C', 'v', '0'};
    auto append16 = [](std::vector<uint8_t>& v, uint16_t x) { v.push_back(uint8_t(x >> 8)); v.push_back(uint8_t(x)); };
    append16(buffer, static_cast<uint16_t>(items.size()));
    const size_t entries_begin = buffer.size() + 2 * (items.size() + 1);
    std::vector<uint8_t> entries;
    for (const auto& item : items) {
        append16(buffer, static_cast<uint16_t>(entries_begin + entries.size()));
        entries.push_back(static_cast<uint8_t>((item.type << 5) | (item.name.size() - 1)));
        entries.insert(entries.end(), item.name.begin(), item.name.end());
        if (item.type == BIG_ARRAY)
            append16(entries, static_cast<uint16_t>(item.bytes.size() - 1));
        else if (item.type == SMALL_ARRAY)
            entries.push_back(static_cast<uint8_t>(item.bytes.size() - 1));
        entries.insert(entries.end(), item.bytes.begin(), item.bytes.end());
    }
    append16(buffer, static_cast<uint16_t>(entries_begin + entries.size()));
    buffer.insert(buffer.end(), entries.begin(), entries.end());
    buffer.resize(SYSCONF_SIZE, 0);
    std::memcpy(buffer.data() + SYSCONF_SIZE - 4, "SCed", 4);
    return buffer;
}

bool sysconf_valid(const std::vector<uint8_t>& data) {
    return data.size() == 0x4000 && std::memcmp(data.data(), "SCv0", 4) == 0 &&
           std::memcmp(data.data() + 0x3FFC, "SCed", 4) == 0;
}

}  // namespace

std::vector<uint8_t> make_default_sysconf(bool widescreen) {
    // BT.DINF (Dolphin ConfPads, 0x461 bytes): num_registered, registered[10],
    // active[5], unknown; each device = bdaddr[6] + name[0x40]. One emulated
    // Wii Remote, as Dolphin's BluetoothEmuDevice writes it (bdaddr reversed).
    std::vector<uint8_t> bt_dinf(0x460 + 1, 0);
    {
        const uint8_t bdaddr[6] = {0x11, 0x02, 0x19, 0x79, 0x00, 0x00};
        const char name[] = "Nintendo RVL-CNT-01";
        constexpr size_t device_size = 6 + 0x40;
        bt_dinf[0] = 1;
        for (size_t base : {size_t(1), size_t(1 + 10 * device_size)}) {
            for (int i = 0; i < 6; ++i) bt_dinf[base + i] = bdaddr[5 - i];
            std::memcpy(bt_dinf.data() + base + 6, name, sizeof(name) - 1);
        }
    }
    std::vector<uint8_t> console_nick = {0, 'd', 0, 'o', 0, 'l', 0, 'p', 0, 'h', 0, 'i', 0, 'n'};
    console_nick.resize(22);
    console_nick[21] = 7;
    std::vector<uint8_t> ipl_sadr(0x1007 + 1, 0);
    ipl_sadr[0] = 0x6c;  // Dolphin default country (USA)
    std::vector<uint8_t> ipl_pc(0x49 + 1, 0);
    ipl_pc[1] = 0x04;
    ipl_pc[2] = 0x14;
    const std::vector<SysConfEntry> items = {
        {BIG_ARRAY, "BT.DINF", bt_dinf},
        {BIG_ARRAY, "BT.CDIF", std::vector<uint8_t>(0x204 + 1, 0)},
        {LONG, "BT.SENS", {0, 0, 0, 3}},
        {BYTE, "BT.BAR", {1}},
        {BYTE, "BT.SPKV", {0x58}},
        {BYTE, "BT.MOT", {1}},
        {SMALL_ARRAY, "IPL.NIK", console_nick},
        {BYTE, "IPL.LNG", {1}},  // English
        {BIG_ARRAY, "IPL.SADR", ipl_sadr},
        {SMALL_ARRAY, "IPL.PC", ipl_pc},
        {LONG, "IPL.CB", {0, 0, 0, 0}},
        {BYTE, "IPL.AR", {static_cast<uint8_t>(widescreen ? 1 : 0)}},
        {BYTE, "IPL.SSV", {1}},
        {BYTE_BOOL, "IPL.CD", {0}},
        {BYTE_BOOL, "IPL.CD2", {0}},
        {BYTE_BOOL, "IPL.EULA", {1}},
        {BYTE, "IPL.UPT", {2}},
        {BYTE, "IPL.PGS", {0}},
        {BYTE, "IPL.E60", {1}},
        {BYTE, "IPL.DH", {0}},
        {LONG, "IPL.INC", {0, 0, 0, 8}},
        {LONG, "IPL.FRC", {0, 0, 0, 0x28}},
        {SMALL_ARRAY, "IPL.IDL", {0, 1}},
        {LONG, "NET.WCFG", {0, 0, 0, 1}},
        {LONG, "NET.CTPC", {0, 0, 0, 0}},
        {BYTE, "WWW.RST", {0}},
        {BYTE_BOOL, "MPLS.MOVIE", {1}},
    };
    return save_sysconf(items);
}

bool sysconf_find_item(const std::vector<uint8_t>& sc, const std::string& name, size_t* data_offset,
                       size_t* data_size) {
    if (sc.size() < 6) return false;
    const uint16_t count = get_be16(sc.data() + 4);
    for (uint16_t i = 0; i < count; ++i) {
        if (6 + 2 * (i + 1) > sc.size()) return false;
        size_t offset = get_be16(sc.data() + 6 + 2 * i);
        if (offset >= sc.size()) return false;
        const uint8_t description = sc[offset];
        const uint8_t type = description >> 5;
        const size_t name_length = (description & 0x1f) + 1;
        if (offset + 1 + name_length > sc.size()) return false;
        const std::string item(reinterpret_cast<const char*>(sc.data() + offset + 1), name_length);
        offset += 1 + name_length;
        size_t size = 0;
        switch (type) {
        case BIG_ARRAY: if (offset + 2 > sc.size()) return false; size = get_be16(sc.data() + offset) + 1u; offset += 2; break;
        case SMALL_ARRAY: if (offset + 1 > sc.size()) return false; size = sc[offset] + 1u; offset += 1; break;
        case BYTE: case BYTE_BOOL: size = 1; break;
        case SHORT: size = 2; break;
        case LONG: size = 4; break;
        case LONG_LONG: size = 8; break;
        default: return false;
        }
        if (item == name) {
            if (offset + size > sc.size()) return false;
            *data_offset = offset;
            *data_size = size;
            return true;
        }
    }
    return false;
}

// ---- setting.txt (Dolphin Common/SettingsHandler.cpp) ---------------------------

namespace {
constexpr uint32_t SETTINGS_SEED = 0x73B5DBFA;
constexpr size_t SETTINGS_SIZE = 0x100;
}  // namespace

std::vector<uint8_t> make_setting_txt(const std::vector<std::pair<std::string, std::string>>& settings) {
    std::vector<uint8_t> buffer(SETTINGS_SIZE, 0);
    size_t position = 0;
    uint32_t key = SETTINGS_SEED;
    auto write_byte = [&](uint8_t b) {
        if (position >= buffer.size()) return;
        buffer[position++] = static_cast<uint8_t>(b ^ key);
        key = (key >> 31) | (key << 1);
    };
    auto write_line = [&](auto& self, const std::string& line) -> void {
        const size_t old_position = position;
        const uint32_t old_key = key;
        for (char c : line) write_byte(static_cast<uint8_t>(c));
        // Nintendo's decoder stops at an encoded NUL; re-key with an LF like Nintendo does.
        if (std::find(buffer.begin() + old_position, buffer.begin() + position, 0) !=
            buffer.begin() + position) {
            key = old_key;
            position = old_position;
            write_byte('\n');
            self(self, line);
        }
    };
    for (const auto& [name, value] : settings) write_line(write_line, name + "=" + value + "\r\n");
    return buffer;
}

std::string decode_setting_txt(const std::vector<uint8_t>& encoded) {
    std::string decoded;
    uint32_t key = SETTINGS_SEED;
    for (uint8_t b : encoded) {
        decoded.push_back(static_cast<char>(b ^ key));
        key = (key >> 31) | (key << 1);
    }
    return decoded;
}

void ensure_system_files(HostFS& fs, int widescreen_override) {
    // SYSCONF (system menu owned, rw for everyone, as Dolphin SysConf::Save writes it).
    const std::string sysconf_path = "/shared2/sys/SYSCONF";
    std::vector<uint8_t> sysconf;
    fs.read_whole_file(sysconf_path, sysconf);
    bool write_sysconf = false;
    if (!sysconf_valid(sysconf)) {
        sysconf = make_default_sysconf(widescreen_override != 0);
        write_sysconf = true;
        std::cout << "[NAND] created default SYSCONF (IPL.AR=" << (widescreen_override != 0 ? 1 : 0)
                  << ", IPL.LNG=English)" << std::endl;
    } else if (widescreen_override >= 0) {
        size_t offset = 0, size = 0;
        if (sysconf_find_item(sysconf, "IPL.AR", &offset, &size) && size == 1 &&
            sysconf[offset] != (widescreen_override ? 1 : 0)) {
            sysconf[offset] = widescreen_override ? 1 : 0;
            write_sysconf = true;
        }
    }
    if (write_sysconf) {
        fs.create_directory(SYSMENU_UID, SYSMENU_GID, "/shared2/sys", 0, RW_ALL);
        const int32_t result = fs.write_file_atomic(SYSMENU_UID, SYSMENU_GID, sysconf_path, sysconf, RW_ALL);
        if (result != FS_SUCCESS) std::cout << "[NAND] SYSCONF write failed: " << result << std::endl;
    }

    // setting.txt in the system menu data directory (Dolphin BS2 emulation).
    const uint32_t sysmenu_uid = get_or_insert_uid(fs, SYSTEM_MENU_TITLE);
    create_title_directories(fs, SYSTEM_MENU_TITLE, sysmenu_uid, SYSMENU_GID, nullptr);
    const std::string settings_path = "/title/00000001/00000002/data/setting.txt";
    std::vector<uint8_t> settings;
    fs.read_whole_file(settings_path, settings);
    if (settings.size() != SETTINGS_SIZE || decode_setting_txt(settings).find("AREA=") == std::string::npos) {
        const std::string& game_id = Config::get().game_id;
        const char region = game_id.size() >= 4 ? game_id[3] : 'E';
        struct Region { const char *area, *video, *game, *code; };
        Region r{"USA", "NTSC", "US", "LU"};
        if (region == 'J') r = {"JPN", "NTSC", "JP", "LJH"};
        else if (region == 'K' || region == 'Q' || region == 'T') r = {"KOR", "NTSC", "KR", "LKH"};
        else if (region != 'E' && region != 'N') r = {"EUR", "PAL", "EU", "LEH"};
        settings = make_setting_txt({{"AREA", r.area},
                                     {"MODEL", std::string("RVL-001(") + r.area + ")"},
                                     {"DVD", "0"},
                                     {"MPCH", "0x7FFE"},
                                     {"CODE", r.code},
                                     {"SERNO", "123456789"},
                                     {"VIDEO", r.video},
                                     {"GAME", r.game}});
        const int32_t result = fs.write_file_atomic(SYSMENU_UID, SYSMENU_GID, settings_path, settings, RW_ALL);
        std::cout << "[NAND] created setting.txt (AREA=" << r.area << ") -> " << result << std::endl;
    }
}

}  // namespace nwii::runtime::nand
