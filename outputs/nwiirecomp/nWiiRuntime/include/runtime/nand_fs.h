#pragma once
// Host-backed Wii NAND (M6.1). The guest file system is a directory on the
// host, laid out like Dolphin's User/Wii folder: guest path "/a/b" maps to
// <root>/a/b (each component escaped with Dolphin's Common::EscapeFileName),
// and per-node metadata (owner, group, attribute, modes) lives in <root>/fst.bin
// in Dolphin's HostFileSystem serialisation. A NAND folder written here can be
// used by Dolphin and vice versa.
//
// HostFS mirrors Dolphin's IOS::HLE::FS::HostFileSystem (Core/IOS/FS/HostBackend/
// FS.cpp and File.cpp) operation by operation, including its result codes.
// It works on host memory only; the /dev/fs device marshals guest buffers.

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace nwii::runtime::nand {

// IOS FS result codes: Dolphin ConvertResult(ResultCode) == -(code + 100).
enum : int32_t {
    FS_SUCCESS = 0,
    FS_EINVAL = -101,        // Invalid
    FS_EACCESS = -102,       // AccessDenied
    FS_ESUPERBLOCK = -103,   // SuperblockWriteFailed
    FS_EEXIST = -105,        // AlreadyExists
    FS_ENOENT = -106,        // NotFound
    FS_EFSTFULL = -107,      // FstFull
    FS_ENOSPACE = -108,      // NoFreeSpace
    FS_ENOHANDLE = -109,     // NoFreeHandle
    FS_ETOODEEP = -110,      // TooManyPathComponents
    FS_EINUSE = -111,        // InUse
    FS_ENOTEMPTY = -115,     // FileNotEmpty
    FS_EUNKNOWN = -117,      // UnknownError
};

enum Mode : uint8_t { MODE_NONE = 0, MODE_READ = 1, MODE_WRITE = 2, MODE_RW = 3 };

struct Modes {
    uint8_t owner = MODE_NONE;
    uint8_t group = MODE_NONE;
    uint8_t other = MODE_NONE;
    bool operator==(const Modes&) const = default;
};

struct Metadata {
    uint32_t uid = 0;
    uint16_t gid = 0;
    uint8_t attribute = 0;
    Modes modes{};
    bool is_file = false;
    uint32_t size = 0;
};

struct NandStats {
    uint32_t cluster_size = 0, free_clusters = 0, used_clusters = 0, bad_clusters = 0,
             reserved_clusters = 0, free_inodes = 0, used_inodes = 0;
};

struct DirectoryStats {
    uint32_t used_clusters = 0;
    uint32_t used_inodes = 0;
};

constexpr uint32_t PID_KERNEL = 0;
constexpr uint32_t SYSMENU_UID = 0x1000;
constexpr uint16_t SYSMENU_GID = 1;
constexpr uint64_t SYSTEM_MENU_TITLE = 0x0000000100000002ull;

// Dolphin Core/IOS/FS/FileSystem.h
constexpr uint32_t CLUSTER_SIZE = 0x4000;
constexpr uint32_t TOTAL_CLUSTERS = 0x7ec0;
constexpr uint32_t RESERVED_CLUSTERS = 0x0300;
constexpr uint32_t USABLE_CLUSTERS = TOTAL_CLUSTERS - RESERVED_CLUSTERS;
constexpr uint32_t TOTAL_INODES = 0x17ff;
constexpr size_t MAX_PATH_DEPTH = 8;
constexpr size_t MAX_PATH_LENGTH = 64;
constexpr size_t MAX_FILENAME_LENGTH = 12;
constexpr int MAX_HANDLES = 16;

bool is_valid_path(std::string_view path);
bool is_valid_non_root_path(std::string_view path);
bool is_valid_filename(std::string_view name);
std::string escape_file_name(const std::string& name);
std::string unescape_file_name(const std::string& name);

class HostFS {
public:
    explicit HostFS(std::filesystem::path root);
    ~HostFS();
    HostFS(const HostFS&) = delete;
    HostFS& operator=(const HostFS&) = delete;

    const std::filesystem::path& root() const { return m_root; }
    std::filesystem::path host_path(const std::string& wii_path) const;

    // Namespace operations (Dolphin HostFileSystem). Return FS_SUCCESS or an FS_E* code.
    int32_t format(uint32_t uid);
    int32_t create_file(uint32_t uid, uint16_t gid, const std::string& path, uint8_t attr, Modes modes);
    int32_t create_directory(uint32_t uid, uint16_t gid, const std::string& path, uint8_t attr, Modes modes);
    // Dolphin FileSystem::CreateFullPath: creates every missing parent of the last '/'.
    int32_t create_full_path(uint32_t uid, uint16_t gid, const std::string& path, uint8_t attr, Modes modes);
    int32_t remove(uint32_t uid, uint16_t gid, const std::string& path);
    int32_t rename(uint32_t uid, uint16_t gid, const std::string& old_path, const std::string& new_path);
    int32_t read_directory(uint32_t uid, uint16_t gid, const std::string& path, std::vector<std::string>& out);
    int32_t get_metadata(uint32_t uid, uint16_t gid, const std::string& path, Metadata& out);
    int32_t set_metadata(uint32_t caller_uid, const std::string& path, uint32_t uid, uint16_t gid,
                         uint8_t attr, Modes modes);
    int32_t get_nand_stats(NandStats& out);
    int32_t get_directory_stats(const std::string& path, DirectoryStats& out);

    // File handles (Dolphin HostBackend/File.cpp). open_file returns a handle index >= 0.
    int32_t open_file(uint32_t uid, uint16_t gid, const std::string& path, uint8_t mode);
    int32_t close(int32_t fd);
    int32_t read(int32_t fd, uint8_t* dst, uint32_t count);
    int32_t write(int32_t fd, const uint8_t* src, uint32_t count);
    int32_t seek(int32_t fd, uint32_t offset, uint32_t whence);
    int32_t file_status(int32_t fd, uint32_t& size, uint32_t& offset);
    int open_handle_count() const;

    // Kernel-side conveniences (boot content). Not reachable from the guest.
    bool read_whole_file(const std::string& path, std::vector<uint8_t>& out);
    // Dolphin-style atomic replace: write /tmp/<name>, then Rename over path.
    int32_t write_file_atomic(uint32_t uid, uint16_t gid, const std::string& path,
                              const std::vector<uint8_t>& data, Modes modes);

private:
    struct FstEntry {
        std::string name;
        Metadata data;
        std::list<FstEntry> children;  // std::list: entry pointers stay valid across inserts
        bool check_permission(uint32_t uid, uint16_t gid, uint8_t mode) const;
    };
    struct Handle {
        bool opened = false;
        std::shared_ptr<std::FILE> file;
        std::string wii_path;
        uint8_t mode = 0;
        uint32_t offset = 0;
    };

    FstEntry* get_entry(const std::string& path);
    void reset_fst();
    void load_fst();
    void save_fst();
    int32_t create_node(uint32_t uid, uint16_t gid, const std::string& path, uint8_t attr,
                        Modes modes, bool is_file);
    bool is_file_opened(const std::string& path) const;
    bool is_directory_in_use(const std::string& path) const;
    std::shared_ptr<std::FILE> open_host_file(const std::filesystem::path& path);
    Handle* get_handle(int32_t fd);

    std::filesystem::path m_root;
    FstEntry m_root_entry;
    // Declared before m_handles: the shared FILE deleters erase from this map.
    std::map<std::filesystem::path, std::weak_ptr<std::FILE>> m_open_files;
    std::array<Handle, MAX_HANDLES> m_handles{};
    mutable std::recursive_mutex m_mutex;
};

// ---- Runtime wiring -------------------------------------------------------
// Root resolution: env NWII_NAND_ROOT, else config [nand] root, else
// <exe dir>/../nand. Input-replay runs (NWII_SS_INPUT_REPLAY) without an
// explicit root use <exe dir>/../nand-replay, wiped at boot, so scripted runs
// always start from a fresh NAND.
std::filesystem::path resolve_root(bool* hermetic);
HostFS& instance();
// Tests: replace the singleton with a NAND rooted at `root` (wipe first if asked).
void reset_instance(const std::filesystem::path& root, bool wipe);

// UID/GID the PPC runs as (Dolphin: uid.sys entry of the running title, TMD group id).
uint32_t ppc_uid();
uint16_t ppc_gid();

// Boot steps, in IOS order. boot_fs = Dolphin FSCore ctor (/tmp reset);
// boot_es = ESCore ctor directories + uid.sys + DIVerify title directories;
// ensure_system_files = SYSCONF (SysConf defaults) and setting.txt (BS2 emu).
void boot_fs(HostFS& fs);
void boot_es(HostFS& fs, uint64_t title_id, uint16_t group_id, const std::filesystem::path& tmd_file);
void ensure_system_files(HostFS& fs, int widescreen_override);

// Generators exposed for the unit harness.
std::vector<uint8_t> make_default_sysconf(bool widescreen);
bool sysconf_find_item(const std::vector<uint8_t>& sysconf, const std::string& name,
                       size_t* data_offset, size_t* data_size);
std::vector<uint8_t> make_setting_txt(const std::vector<std::pair<std::string, std::string>>& settings);
std::string decode_setting_txt(const std::vector<uint8_t>& encoded);

}  // namespace nwii::runtime::nand
