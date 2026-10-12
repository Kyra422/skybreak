// /dev/fs (ISFS) on top of the host-backed NAND (runtime/nand_fs.h).
// Request decoding, vector checks and result codes follow Dolphin's
// FSDevice (Source/Core/Core/IOS/FS/FileSystemProxy.cpp); the file system
// operations themselves are nand::HostFS (Dolphin HostFileSystem).
#include "runtime/devices.h"
#include "runtime/nand_fs.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

namespace nwii::runtime::devices {

namespace {

using namespace nwii::runtime::nand;

constexpr int32_t CONTROL_FD = 0;     // device-local fd of every "/dev/fs" handle
constexpr int32_t FILE_FD_BASE = 20;  // device-local fd of a file = base + HostFS handle
constexpr uint32_t ISFS_PARAMS_SIZE = 0x4A;  // Dolphin ISFSParams (pack 1): uid, gid, path[64], modes[3], attr
constexpr uint32_t NAND_STATS_SIZE = 0x1C;   // Dolphin ISFSNandStats: 7 x u32
constexpr int MAX_FS_FDS = 16;               // Dolphin FSCore::Open: m_fd_map.size() >= 16

enum : uint32_t {
    ISFS_IOCTL_FORMAT = 1,
    ISFS_IOCTL_GETSTATS = 2,
    ISFS_IOCTL_CREATEDIR = 3,
    ISFS_IOCTLV_READDIR = 4,
    ISFS_IOCTL_SETATTR = 5,
    ISFS_IOCTL_GETATTR = 6,
    ISFS_IOCTL_DELETE = 7,
    ISFS_IOCTL_RENAME = 8,
    ISFS_IOCTL_CREATEFILE = 9,
    ISFS_IOCTL_SETFILEVERCTRL = 10,
    ISFS_IOCTL_GETFILESTATS = 11,
    ISFS_IOCTLV_GETUSAGE = 12,
    ISFS_IOCTL_SHUTDOWN = 13,
};

// IPC buffers carry physical (or cached/uncached) MEM1/MEM2 addresses.
uint8_t* guest_range(CPUContext& ctx, uint32_t address, uint32_t size) {
    if (address == 0 || size == 0) return nullptr;
    if ((address & 0x3FFFFFFF) >= 0x0C000000 && (address & 0x3FFFFFFF) < 0x10000000) return nullptr;
    return ctx.mmu.get_ptr(address & 0x3FFFFFFF, size);
}

// Dolphin Memory::GetString(address, 64): up to 64 bytes, stops at NUL.
bool guest_string(CPUContext& ctx, uint32_t address, uint32_t max_length, std::string& out) {
    const uint8_t* p = guest_range(ctx, address, max_length);
    if (!p) return false;
    const size_t length = strnlen(reinterpret_cast<const char*>(p), max_length);
    out.assign(reinterpret_cast<const char*>(p), length);
    return true;
}

void put_be32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}

uint32_t get_be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

struct IsfsParams {
    uint32_t uid = 0;
    uint16_t gid = 0;
    std::string path;
    Modes modes{};
    uint8_t attribute = 0;
};

bool read_params(CPUContext& ctx, const IpcRequest& req, IsfsParams& out) {
    if (req.in_size < ISFS_PARAMS_SIZE) return false;
    const uint8_t* p = guest_range(ctx, req.in_buf, ISFS_PARAMS_SIZE);
    if (!p) return false;
    out.uid = get_be32(p);
    out.gid = static_cast<uint16_t>((p[4] << 8) | p[5]);
    out.path.assign(reinterpret_cast<const char*>(p + 6), strnlen(reinterpret_cast<const char*>(p + 6), 64));
    out.modes = {p[0x46], p[0x47], p[0x48]};
    out.attribute = p[0x49];
    return true;
}

// Dolphin IOCtlVRequest::HasNumberOfValidVectors.
bool has_vectors(const IpcRequest& req, uint32_t in, uint32_t io) {
    if (req.arg_cnt_in != in || req.arg_cnt_out != io || req.ioctlv_vecs.size() < in + io) return false;
    for (uint32_t i = 0; i < in + io; ++i)
        if (req.ioctlv_vecs[i].len != 0 && req.ioctlv_vecs[i].addr == 0) return false;
    return true;
}

}  // namespace

class FSDevice : public IDevice {
public:
    FSDevice() { boot_fs(instance()); }

    const char* get_name() const override { return "/dev/fs"; }
    bool matches_path(const std::string& path) const override {
        if (path == "/dev/fs") return true;
        if (path.find("/dev/") == 0) return false;
        return path.find("/") == 0;
    }

    int32_t open(CPUContext&, const std::string& path, uint32_t mode) override {
        std::lock_guard lock(mutex_);
        HostFS& fs = instance();
        if (control_handles_ + fs.open_handle_count() >= MAX_FS_FDS) return FS_ENOHANDLE;
        if (path.size() >= 64) return FS_EINVAL;
        if (path == "/dev/fs") {
            ++control_handles_;
            return CONTROL_FD;
        }
        const int32_t fd = fs.open_file(ppc_uid(), ppc_gid(), path, static_cast<uint8_t>(mode & 3));
        if (fd < 0) {
            std::cout << "[FS] Open(" << path << ", " << (mode & 3) << ") -> " << fd << std::endl;
            return fd;
        }
        return FILE_FD_BASE + fd;
    }

    int32_t close(CPUContext&, uint32_t fd) override {
        std::lock_guard lock(mutex_);
        if (static_cast<int32_t>(fd) == CONTROL_FD) {
            if (control_handles_ > 0) --control_handles_;
            return IPC_OK;
        }
        return instance().close(static_cast<int32_t>(fd) - FILE_FD_BASE);
    }

    int32_t read(CPUContext& ctx, uint32_t fd, uint32_t buf, uint32_t len) override {
        std::lock_guard lock(mutex_);
        if (static_cast<int32_t>(fd) == CONTROL_FD) return FS_EINVAL;
        uint8_t* dst = guest_range(ctx, buf, len);
        if (len && !dst) return FS_EINVAL;
        const int32_t result = instance().read(static_cast<int32_t>(fd) - FILE_FD_BASE, dst, len);
        if (result < 0) std::cout << "[FS] Read(fd " << fd << ", " << len << ") -> " << result << std::endl;
        return result;
    }

    int32_t write(CPUContext& ctx, uint32_t fd, uint32_t buf, uint32_t len) override {
        std::lock_guard lock(mutex_);
        if (static_cast<int32_t>(fd) == CONTROL_FD) return FS_EINVAL;
        const uint8_t* src = guest_range(ctx, buf, len);
        if (len && !src) return FS_EINVAL;
        const int32_t result = instance().write(static_cast<int32_t>(fd) - FILE_FD_BASE, src, len);
        if (result < 0) std::cout << "[FS] Write(fd " << fd << ", " << len << ") -> " << result << std::endl;
        return result;
    }

    int32_t seek(CPUContext&, uint32_t fd, int32_t offset, int32_t whence) override {
        std::lock_guard lock(mutex_);
        if (static_cast<int32_t>(fd) == CONTROL_FD) return FS_EINVAL;
        return instance().seek(static_cast<int32_t>(fd) - FILE_FD_BASE, static_cast<uint32_t>(offset),
                               static_cast<uint32_t>(whence));
    }

    int32_t ioctl(CPUContext& ctx, const IpcRequest& req) override {
        std::lock_guard lock(mutex_);
        HostFS& fs = instance();
        const uint32_t uid = ppc_uid();
        const uint16_t gid = ppc_gid();
        std::string path, path2;
        IsfsParams params;
        int32_t result = FS_EINVAL;
        switch (req.ioctl_cmd) {
        case ISFS_IOCTL_FORMAT:
            result = uid != 0 ? FS_EACCESS : fs.format(uid);
            log("Format", "/", result);
            return result;
        case ISFS_IOCTL_GETSTATS: {
            uint8_t* out = guest_range(ctx, req.out_buf, NAND_STATS_SIZE);
            if (req.out_size < NAND_STATS_SIZE || !out) return FS_EINVAL;
            NandStats stats;
            result = fs.get_nand_stats(stats);
            if (result != FS_SUCCESS) return result;
            const uint32_t words[7] = {stats.cluster_size, stats.free_clusters, stats.used_clusters,
                                       stats.bad_clusters, stats.reserved_clusters, stats.free_inodes,
                                       stats.used_inodes};
            for (int i = 0; i < 7; ++i) put_be32(out + 4 * i, words[i]);
            return FS_SUCCESS;
        }
        case ISFS_IOCTL_CREATEDIR:
            if (!read_params(ctx, req, params)) return FS_EINVAL;
            result = fs.create_directory(uid, gid, params.path, params.attribute, params.modes);
            log("CreateDir", params.path, result);
            return result;
        case ISFS_IOCTL_CREATEFILE:
            if (!read_params(ctx, req, params)) return FS_EINVAL;
            result = fs.create_file(uid, gid, params.path, params.attribute, params.modes);
            log("CreateFile", params.path, result);
            return result;
        case ISFS_IOCTL_SETATTR:
            if (!read_params(ctx, req, params)) return FS_EINVAL;
            result = fs.set_metadata(uid, params.path, params.uid, params.gid, params.attribute, params.modes);
            log("SetAttr", params.path, result);
            return result;
        case ISFS_IOCTL_GETATTR: {
            uint8_t* out = guest_range(ctx, req.out_buf, ISFS_PARAMS_SIZE);
            if (req.in_size < 64 || req.out_size < ISFS_PARAMS_SIZE || !out ||
                !guest_string(ctx, req.in_buf, 64, path))
                return FS_EINVAL;
            Metadata metadata;
            result = fs.get_metadata(uid, gid, path, metadata);
            log("GetAttr", path, result);
            if (result != FS_SUCCESS) return result;
            // IOS copies only these members (Dolphin zero-fills the rest for determinism).
            std::memset(out, 0, ISFS_PARAMS_SIZE);
            put_be32(out, metadata.uid);
            out[4] = uint8_t(metadata.gid >> 8);
            out[5] = uint8_t(metadata.gid);
            out[0x46] = metadata.modes.owner;
            out[0x47] = metadata.modes.group;
            out[0x48] = metadata.modes.other;
            out[0x49] = metadata.attribute;
            return FS_SUCCESS;
        }
        case ISFS_IOCTL_DELETE:
            if (req.in_size < 64 || !guest_string(ctx, req.in_buf, 64, path)) return FS_EINVAL;
            result = fs.remove(uid, gid, path);
            log("Delete", path, result);
            return result;
        case ISFS_IOCTL_RENAME:
            if (req.in_size < 128 || !guest_string(ctx, req.in_buf, 64, path) ||
                !guest_string(ctx, req.in_buf + 64, 64, path2))
                return FS_EINVAL;
            result = fs.rename(uid, gid, path, path2);
            log("Rename", path + " -> " + path2, result);
            return result;
        case ISFS_IOCTL_SETFILEVERCTRL:
            if (!read_params(ctx, req, params)) return FS_EINVAL;
            log("SetFileVersionControl (stub)", params.path, FS_SUCCESS);
            return FS_SUCCESS;
        case ISFS_IOCTL_GETFILESTATS: {
            uint8_t* out = guest_range(ctx, req.out_buf, 8);
            if (req.out_size < 8 || !out) return FS_EINVAL;
            if (static_cast<int32_t>(req.fd) == CONTROL_FD) return FS_EINVAL;
            uint32_t size = 0, offset = 0;
            result = fs.file_status(static_cast<int32_t>(req.fd) - FILE_FD_BASE, size, offset);
            if (result != FS_SUCCESS) return result;
            put_be32(out, size);
            put_be32(out + 4, offset);
            return FS_SUCCESS;
        }
        case ISFS_IOCTL_SHUTDOWN:
            log("Shutdown", "", FS_SUCCESS);
            return FS_SUCCESS;
        default:
            std::cout << "[FS] unsupported ioctl " << req.ioctl_cmd << " -> -101" << std::endl;
            return FS_EINVAL;
        }
    }

    int32_t ioctlv(CPUContext& ctx, const IpcRequest& req) override {
        std::lock_guard lock(mutex_);
        switch (req.ioctl_cmd) {
        case ISFS_IOCTLV_READDIR: return read_directory(ctx, req);
        case ISFS_IOCTLV_GETUSAGE: return get_usage(ctx, req);
        default:
            std::cout << "[FS] unsupported ioctlv " << req.ioctl_cmd << " -> -101" << std::endl;
            return FS_EINVAL;
        }
    }

private:
    static void log(const char* op, const std::string& path, int32_t result) {
        std::cout << "[FS] " << op << "(" << path << ") -> " << result << std::endl;
    }

    int32_t read_directory(CPUContext& ctx, const IpcRequest& req) {
        const uint32_t in = req.arg_cnt_in;
        if (in == 0 || in != req.arg_cnt_out || in > 2 || req.ioctlv_vecs.size() < 2 * in ||
            req.ioctlv_vecs[0].len != 64)
            return FS_EINVAL;
        const IoctlvVector* io = req.ioctlv_vecs.data() + in;
        uint32_t list_address = 0, count_address = 0, max_count = 0;
        if (in == 2) {
            if (req.ioctlv_vecs[1].len != 4 || io[1].len != 4) return FS_EINVAL;
            const uint8_t* max_ptr = guest_range(ctx, req.ioctlv_vecs[1].addr, 4);
            if (!max_ptr || !guest_range(ctx, io[1].addr, 4)) return FS_EINVAL;
            max_count = get_be32(max_ptr);
            count_address = io[1].addr;
            list_address = io[0].addr;
            if (uint64_t(io[0].len) != 13ull * max_count) return FS_EINVAL;
            if (max_count && !guest_range(ctx, list_address, io[0].len)) return FS_EINVAL;
            put_be32(guest_range(ctx, count_address, 4), max_count);
        } else {
            if (io[0].len != 4 || !guest_range(ctx, io[0].addr, 4)) return FS_EINVAL;
            count_address = io[0].addr;
        }
        std::string path;
        if (!guest_string(ctx, req.ioctlv_vecs[0].addr, 64, path)) return FS_EINVAL;
        std::vector<std::string> list;
        const int32_t result = instance().read_directory(ppc_uid(), ppc_gid(), path, list);
        std::cout << "[FS] ReadDir(" << path << (list_address ? ", list" : ", count") << ") -> " << result;
        if (result == FS_SUCCESS) std::cout << " entries=" << list.size();
        std::cout << std::endl;
        if (result != FS_SUCCESS) return result;
        uint8_t* count_ptr = guest_range(ctx, count_address, 4);
        if (!list_address) {
            put_be32(count_ptr, static_cast<uint32_t>(list.size()));
            return FS_SUCCESS;
        }
        // Dolphin packs the names (stride = length + 1) after zeroing 13 bytes per entry.
        uint8_t* base = guest_range(ctx, list_address, io[0].len);
        size_t position = 0;
        for (size_t i = 0; i < list.size() && i < max_count; ++i) {
            std::memset(base + position, 0, 13);
            std::memcpy(base + position, list[i].data(), list[i].size());
            base[position + 12] = 0;
            position += list[i].size() + 1;
        }
        put_be32(count_ptr, std::min<uint32_t>(max_count, static_cast<uint32_t>(list.size())));
        return FS_SUCCESS;
    }

    int32_t get_usage(CPUContext& ctx, const IpcRequest& req) {
        if (!has_vectors(req, 1, 2) || req.ioctlv_vecs[0].len != 64 || req.ioctlv_vecs[1].len != 4 ||
            req.ioctlv_vecs[2].len != 4)
            return FS_EINVAL;
        uint8_t* clusters = guest_range(ctx, req.ioctlv_vecs[1].addr, 4);
        uint8_t* inodes = guest_range(ctx, req.ioctlv_vecs[2].addr, 4);
        std::string path;
        if (!clusters || !inodes || !guest_string(ctx, req.ioctlv_vecs[0].addr, 64, path)) return FS_EINVAL;
        DirectoryStats stats;
        const int32_t result = instance().get_directory_stats(path, stats);
        std::cout << "[FS] GetUsage(" << path << ") -> " << result << " clusters=" << stats.used_clusters
                  << " inodes=" << stats.used_inodes << std::endl;
        if (result != FS_SUCCESS) return result;
        put_be32(clusters, stats.used_clusters);
        put_be32(inodes, stats.used_inodes);
        return FS_SUCCESS;
    }

    int control_handles_ = 0;
    std::recursive_mutex mutex_;
};

std::unique_ptr<IDevice> create_fs_device() {
    return std::make_unique<FSDevice>();
}

}  // namespace nwii::runtime::devices
