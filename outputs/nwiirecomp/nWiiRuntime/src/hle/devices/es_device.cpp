// /dev/es. Implements the ES ioctlvs Skyward Sword issues, with Dolphin's
// vector checks and results (Source/Core/Core/IOS/ES/ES.cpp, Views.cpp).
// Unknown ioctlvs return IPC_EINVAL like Dolphin's default case.
// Construction also performs the ES boot work on the NAND (Dolphin ESCore
// ctor + DIVerify for the disc title) and creates the BS2 system files.
#include "runtime/devices.h"
#include "runtime/ios_kernel.h"
#include "runtime/config.h"
#include "runtime/nand_fs.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>

namespace nwii::runtime::devices {

namespace {

constexpr int32_t ES_EINVAL = -1017;

enum : uint32_t {
    IOCTL_ES_GETCONSUMPTION = 0x16,
    IOCTL_ES_DIGETTICKETVIEW = 0x1B,
    IOCTL_ES_GETTITLEDIR = 0x1D,
    IOCTL_ES_GETDEVICECERT = 0x1E,
    IOCTL_ES_GETTITLEID = 0x20,
};

// REL22-CERT: /dev/es GetDeviceCert answers with a GENERATED placeholder, never a real console's certificate (until
// 11 Oct 2026 this file embedded the Nintendo-signed device certificate of one real Wii). Layout = the IOS ECC device
// certificate (Dolphin IOS::ES CertHeader + CertECC, 0x180 bytes):
//   0x000 signature type 0x00010002 (ECC)  0x004 signature[0x3C]  0x040 padding[0x40]  0x080 issuer[0x40]
//   0x0C0 key type 2 (ECC)  0x0C4 name[0x40] "NG%08x"  0x104 key id  0x108 public key[0x3C]  0x144 padding[0x3C]
// Signature, key id and public key stay ZERO and the device id is the placeholder 0, so it identifies no Wii and can never
// verify against Nintendo's MS key. Skyward Sword never asks for it: its ES library (revolution/esp) has no
// GetDeviceCert/GetDeviceId/Sign, and the REL22-CERT boot/file-select/save/reload logs show only 0x20 0x1D 0x1B 0x16.
// The handler stays (Dolphin parity), so a caller gets a well-formed certificate instead of an error.
constexpr uint32_t PLACEHOLDER_DEVICE_ID = 0x00000000;
constexpr size_t DEVICE_CERT_SIZE = 0x180;

void put_be32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}

std::array<uint8_t, DEVICE_CERT_SIZE> make_placeholder_device_cert() {
    std::array<uint8_t, DEVICE_CERT_SIZE> cert{};  // everything not written below stays zero
    put_be32(cert.data() + 0x000, 0x00010002);
    static constexpr char kIssuer[] = "Root-CA00000001-MS00000002";
    std::memcpy(cert.data() + 0x080, kIssuer, sizeof(kIssuer) - 1);
    put_be32(cert.data() + 0x0C0, 2);
    char name[16];
    std::snprintf(name, sizeof(name), "NG%08x", PLACEHOLDER_DEVICE_ID);
    std::memcpy(cert.data() + 0x0C4, name, std::strlen(name));
    return cert;
}

uint64_t running_title_id() {
    const std::string& gid = Config::get().game_id;
    uint32_t low = 0;
    for (size_t i = 0; i < 4 && i < gid.size(); i++)
        low |= static_cast<uint32_t>(static_cast<uint8_t>(gid[i])) << ((3 - i) * 8);
    return (uint64_t(0x00010000) << 32) | low;
}

// TMD group id = disc maker code (game id bytes 4-5), e.g. "01" -> 0x3031.
uint16_t running_group_id() {
    const std::string& gid = Config::get().game_id;
    if (gid.size() >= 6)
        return static_cast<uint16_t>((static_cast<uint8_t>(gid[4]) << 8) | static_cast<uint8_t>(gid[5]));
    return 0x3031;
}

uint8_t* guest_range(CPUContext& ctx, uint32_t address, uint32_t size) {
    if (address == 0 || size == 0) return nullptr;
    return ctx.mmu.get_ptr(address & 0x3FFFFFFF, size);
}

// Dolphin IOCtlVRequest::HasNumberOfValidVectors.
bool has_vectors(const IpcRequest& req, uint32_t in, uint32_t io) {
    if (req.arg_cnt_in != in || req.arg_cnt_out != io || req.ioctlv_vecs.size() < in + io) return false;
    for (uint32_t i = 0; i < in + io; ++i)
        if (req.ioctlv_vecs[i].len != 0 && req.ioctlv_vecs[i].addr == 0) return false;
    return true;
}

}  // namespace

class ESDevice : public IDevice {
public:
    ESDevice() {
        auto& fs = nand::instance();
        const std::string& game_dir = Config::get().game_dir;
        const std::filesystem::path tmd = game_dir.empty() ? std::filesystem::path{}
                                                           : std::filesystem::path(game_dir) / "tmd.bin";
        nand::boot_es(fs, running_title_id(), running_group_id(), tmd);
        nand::ensure_system_files(fs, Config::get().nand_widescreen);
    }

    const char* get_name() const override { return "/dev/es"; }
    bool matches_path(const std::string& path) const override { return path == get_name(); }

    // ES has no plain ioctls (Dolphin falls through to Device::IOCtl).
    int32_t ioctl(CPUContext&, const IpcRequest& req) override {
        std::cout << "[ES] ioctl cmd=" << req.ioctl_cmd << " -> IPC_EINVAL" << std::endl;
        return IPC_EINVAL;
    }

    int32_t ioctlv(CPUContext& ctx, const IpcRequest& req) override {
        std::cout << "[ES] ioctlv cmd=0x" << std::hex << req.ioctl_cmd << std::dec
                  << " in=" << req.arg_cnt_in << " out=" << req.arg_cnt_out << std::endl;
        switch (req.ioctl_cmd) {
        case IOCTL_ES_GETTITLEID: {
            if (!has_vectors(req, 0, 1) || req.ioctlv_vecs[0].len < 8) return ES_EINVAL;
            uint8_t* out = guest_range(ctx, req.ioctlv_vecs[0].addr, 8);
            if (!out) return ES_EINVAL;
            const uint64_t tid = running_title_id();
            put_be32(out, uint32_t(tid >> 32));
            put_be32(out + 4, uint32_t(tid));
            std::cout << "[ES] Returned Title ID " << std::hex << uint32_t(tid >> 32) << "-"
                      << uint32_t(tid) << std::dec << " (" << Config::get().game_id << ")\n";
            return IPC_OK;
        }
        case IOCTL_ES_GETTITLEDIR: {
            // Dolphin ESDevice::GetTitleDirectory: "/title/%08x/%08x/data" for the given title.
            if (!has_vectors(req, 1, 1) || req.ioctlv_vecs[0].len < 8) return ES_EINVAL;
            const uint8_t* tid_ptr = guest_range(ctx, req.ioctlv_vecs[0].addr, 8);
            const IoctlvVector& out_vec = req.ioctlv_vecs[1];
            uint8_t* out = guest_range(ctx, out_vec.addr, out_vec.len);
            if (!tid_ptr || !out) return ES_EINVAL;
            const uint32_t high = (uint32_t(tid_ptr[0]) << 24) | (uint32_t(tid_ptr[1]) << 16) |
                                  (uint32_t(tid_ptr[2]) << 8) | tid_ptr[3];
            const uint32_t low = (uint32_t(tid_ptr[4]) << 24) | (uint32_t(tid_ptr[5]) << 16) |
                                 (uint32_t(tid_ptr[6]) << 8) | tid_ptr[7];
            char path[64];
            std::snprintf(path, sizeof(path), "/title/%08x/%08x/data", high, low);
            const uint32_t length = static_cast<uint32_t>(std::strlen(path));
            if (out_vec.len < length) return ES_EINVAL;
            std::memset(out, 0, out_vec.len);
            std::memcpy(out, path, length);
            std::cout << "[ES] GetTitleDir -> " << path << std::endl;
            return IPC_OK;
        }
        case IOCTL_ES_GETDEVICECERT: {
            // REL22-CERT: the generated placeholder above, not a real console's certificate.
            if (!has_vectors(req, 0, 1) || req.ioctlv_vecs[0].len < DEVICE_CERT_SIZE) return ES_EINVAL;
            uint8_t* out = guest_range(ctx, req.ioctlv_vecs[0].addr, DEVICE_CERT_SIZE);
            if (!out) return ES_EINVAL;
            const auto cert = make_placeholder_device_cert();
            std::memcpy(out, cert.data(), cert.size());
            std::cout << "[ES] GetDeviceCert -> placeholder certificate (no real console)" << std::endl;
            return IPC_OK;
        }
        case IOCTL_ES_DIGETTICKETVIEW: {
            // Ticket view of the running disc title (no ticket passed in), as before:
            // title id at +0x10, access mask 0xFFFF at +0x18, rest zero.
            if (!has_vectors(req, 1, 1)) return ES_EINVAL;
            const IoctlvVector& out_vec = req.ioctlv_vecs[1];
            const uint32_t size = std::min<uint32_t>(out_vec.len, 0x10000);
            uint8_t* out = guest_range(ctx, out_vec.addr, size);
            if (!out || size < 0x1A) return ES_EINVAL;
            std::memset(out, 0, size);
            const uint64_t tid = running_title_id();
            put_be32(out + 0x10, uint32_t(tid >> 32));
            put_be32(out + 0x14, uint32_t(tid));
            out[0x18] = 0xFF;
            out[0x19] = 0xFF;
            std::cout << "[ES] Returning TicketView for TitleID " << std::hex << uint32_t(tid >> 32) << "-"
                      << uint32_t(tid) << std::dec << std::endl;
            return IPC_OK;
        }
        case IOCTL_ES_GETCONSUMPTION: {
            // Dolphin (after crediar's ES module): zero consumption records.
            if (!has_vectors(req, 1, 2)) return ES_EINVAL;
            uint8_t* count = guest_range(ctx, req.ioctlv_vecs[2].addr, 4);
            if (!count) return ES_EINVAL;
            put_be32(count, 0);
            return IPC_OK;
        }
        default:
            std::cout << "[ES] unhandled ioctlv 0x" << std::hex << req.ioctl_cmd << std::dec
                      << " -> IPC_EINVAL" << std::endl;
            return IPC_EINVAL;
        }
    }
};

std::unique_ptr<IDevice> create_es_device() {
    return std::make_unique<ESDevice>();
}

void register_all() {
    auto& kernel = IOSKernel::get();
    kernel.register_device(create_di_device());
    kernel.register_device(create_fs_device());
    kernel.register_device(create_stm_device());
    kernel.register_device(create_usb_device());
    kernel.register_device(create_es_device());
}

}  // namespace nwii::runtime::devices
