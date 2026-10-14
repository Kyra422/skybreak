// REL22-CERT: /dev/es after the real Wii device certificate was replaced by a generated placeholder
// (nWiiRuntime/src/hle/devices/es_device.cpp, make_placeholder_device_cert). Drives /dev/es through IOSKernel exactly
// like the IPC path does (same harness shape as nand/nand-test.cpp), on a temp NAND under work/rel22cert/tests/root.
//   C1 GetDeviceCert (0x1E, 0 in / 1 out of 0x180) -> IPC_OK
//   C2 header: signature type 0x00010002, issuer "Root-CA00000001-MS00000002" (rest of the field zero), key type 2
//   C3 signature + padding (0x004..0x080) all zero: can never verify against Nintendo's MS key
//   C4 name = "NG00000000" (placeholder device id 0), rest of the name field zero
//   C5 key id + public key + tail padding (0x104..0x180) all zero
//   C6 the whole 0x180-byte buffer is written (0xAA prefill gone), two calls and a call after an IOS reload are identical
//   C7 short buffer (0x17F) and a wrong vector count -> ES_EINVAL (-1017), buffer untouched
//   C8 the ioctlvs Skyward Sword really issues still work: 0x20 GetTitleId, 0x1D GetTitleDir, 0x1B DIGetTicketView,
//      0x16 GetConsumption
//   C9 GetDeviceId (0x07) and Sign (0x30) stay unimplemented (IPC_EINVAL): no other identity or signature is handed out
// Output: "PASS: Cn ..." / "FAIL: Cn ..." lines and "RESULT pass=N fail=M"; exit 0 only when every check passes.
// Negative control: build-es-cert-test.cmd --mutate (mk-es-cert-mutant.py) -> C3 C4 C5 C7 must FAIL.
#include "runtime/config.h"
#include "runtime/devices.h"
#include "runtime/ios_kernel.h"
#include "runtime/nand_fs.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace nwii::runtime;
namespace nd = nwii::runtime::nand;
namespace stdfs = std::filesystem;

namespace nwii::runtime {
uint32_t g_watch_addr = 0;
void watch_hit(uint32_t, uint32_t, int) {}
void GX_WGPIPE_Write8(uint8_t) {}
void GX_WGPIPE_Write16(uint16_t) {}
void GX_WGPIPE_Write32(uint32_t) {}
void GX_WGPIPE_WriteF32(float) {}
void HW_Reg_Write16(uint32_t, uint16_t) {}
void HW_Reg_Write32(uint32_t, uint32_t) {}
uint16_t HW_Reg_Read16(uint32_t) { return 0; }
uint32_t HW_Reg_Read32(uint32_t) { return 0; }
}  // namespace nwii::runtime
namespace nwii::runtime::devices {
// register_all() (es_device.cpp) references these; the harness does not call it.
std::unique_ptr<IDevice> create_di_device() { return nullptr; }
std::unique_ptr<IDevice> create_stm_device() { return nullptr; }
std::unique_ptr<IDevice> create_usb_device() { return nullptr; }
}  // namespace nwii::runtime::devices

namespace {

int g_pass = 0, g_fail = 0;
void check(const char* id, bool ok, const std::string& what) {
    (ok ? g_pass : g_fail)++;
    std::cout << (ok ? "PASS: " : "FAIL: ") << id << " " << what << std::endl;
}

constexpr int32_t ES_EINVAL = -1017;
constexpr uint32_t CERT_SIZE = 0x180;
// MEM2 physical addresses, as IPC hands them to IOS.
constexpr uint32_t A_OUT = 0x10003000, A_OUT2 = 0x10003400, A_VEC1 = 0x10004000, A_VEC2 = 0x10004100;

CPUContext* g_ctx;
IOSKernel* g_kernel;

uint8_t* mem(uint32_t address, uint32_t size = 1) { return g_ctx->mmu.get_ptr(address, size); }
uint32_t be32(uint32_t address) { return g_ctx->mmu.read32(address); }
void put_be32(uint32_t address, uint32_t value) { g_ctx->mmu.write32(address, value); }

int32_t ioctlv(int32_t fd, uint32_t cmd, std::vector<IoctlvVector> in, std::vector<IoctlvVector> io) {
    IpcRequest req{};
    req.cmd = 7;
    req.fd = static_cast<uint32_t>(fd);
    req.ioctl_cmd = cmd;
    req.arg_cnt_in = static_cast<uint32_t>(in.size());
    req.arg_cnt_out = static_cast<uint32_t>(io.size());
    req.ioctlv_vecs = in;
    req.ioctlv_vecs.insert(req.ioctlv_vecs.end(), io.begin(), io.end());
    return g_kernel->ioctlv(*g_ctx, req);
}

const stdfs::path kWork = stdfs::path("work") / "rel22cert" / "tests";
const stdfs::path kRoot = kWork / "root";

int32_t boot_es(bool wipe) {
    nd::reset_instance(kRoot, wipe);
    g_kernel->init();  // drops the old devices (IOS reload)
    g_kernel->register_device(devices::create_fs_device());
    g_kernel->register_device(devices::create_es_device());
    return g_kernel->open(*g_ctx, "/dev/es", 0);
}

bool all_zero(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (p[i]) return false;
    return true;
}
bool all_byte(const uint8_t* p, size_t n, uint8_t v) {
    for (size_t i = 0; i < n; ++i)
        if (p[i] != v) return false;
    return true;
}

std::vector<uint8_t> get_cert(int32_t es, uint32_t addr, int32_t* rc) {
    std::memset(mem(addr, CERT_SIZE), 0xAA, CERT_SIZE);
    *rc = ioctlv(es, 0x1E, {}, {{addr, CERT_SIZE}});
    const uint8_t* p = mem(addr, CERT_SIZE);
    return std::vector<uint8_t>(p, p + CERT_SIZE);
}

}  // namespace

int main() {
    auto ctx = std::make_unique<CPUContext>();
    g_ctx = ctx.get();
    g_kernel = &IOSKernel::get();
    Config::get().game_id = "SOUE01";
    stdfs::create_directories(kWork / "game");
    {
        std::ofstream tmd(kWork / "game" / "tmd.bin", std::ios::binary);
        tmd << "fake-tmd-bytes";
    }
    Config::get().game_dir = (kWork / "game").string();
    _putenv_s("NWII_NAND_ROOT", "");

    int32_t es = boot_es(true);
    if (es <= 0) {
        check("C1", false, "open /dev/es failed");
        std::cout << "RESULT pass=" << g_pass << " fail=" << g_fail << std::endl;
        return 1;
    }

    int32_t rc = 0;
    const std::vector<uint8_t> c = get_cert(es, A_OUT, &rc);
    check("C1", rc == 0, "GetDeviceCert 0x1E -> " + std::to_string(rc) + " (expected 0)");

    const uint8_t* p = c.data();
    auto be = [&](size_t o) { return (uint32_t(p[o]) << 24) | (uint32_t(p[o + 1]) << 16) | (uint32_t(p[o + 2]) << 8) | p[o + 3]; };
    const char kIssuer[] = "Root-CA00000001-MS00000002";
    const bool issuer_ok = std::memcmp(p + 0x80, kIssuer, sizeof(kIssuer) - 1) == 0 &&
                           all_zero(p + 0x80 + sizeof(kIssuer) - 1, 0x40 - (sizeof(kIssuer) - 1));
    check("C2", be(0) == 0x00010002u && issuer_ok && be(0xC0) == 2u,
          "ECC header: sig type / issuer / key type (sig type " + std::to_string(be(0)) + ", key type " +
              std::to_string(be(0xC0)) + ")");
    check("C3", all_zero(p + 0x04, 0x7C), "signature + padding 0x004..0x080 all zero (no Nintendo signature)");
    const char kName[] = "NG00000000";
    const bool name_ok = std::memcmp(p + 0xC4, kName, sizeof(kName) - 1) == 0 &&
                         all_zero(p + 0xC4 + sizeof(kName) - 1, 0x40 - (sizeof(kName) - 1));
    check("C4", name_ok, std::string("name = placeholder NG00000000 (got '") +
                             std::string(reinterpret_cast<const char*>(p + 0xC4), strnlen(reinterpret_cast<const char*>(p + 0xC4), 0x40)) + "')");
    check("C5", all_zero(p + 0x104, CERT_SIZE - 0x104), "key id + public key + padding 0x104..0x180 all zero");

    // C6: every byte written; stable across calls and an IOS reload
    bool full = true;
    for (size_t i = 0; i + 4 <= CERT_SIZE; i += 4)
        if (all_byte(p + i, 4, 0xAA)) full = false;
    int32_t rc2 = 0, rc3 = 0;
    const std::vector<uint8_t> c2 = get_cert(es, A_OUT2, &rc2);
    g_kernel->close(*g_ctx, es);
    es = boot_es(false);
    const std::vector<uint8_t> c3 = get_cert(es, A_OUT, &rc3);
    check("C6", full && rc2 == 0 && rc3 == 0 && c == c2 && c == c3,
          "whole buffer written, identical on a second call and after an IOS reload");

    // C7: vector checks
    std::memset(mem(A_OUT, CERT_SIZE), 0xAA, CERT_SIZE);
    const int32_t short_rc = ioctlv(es, 0x1E, {}, {{A_OUT, CERT_SIZE - 1}});
    const bool short_untouched = all_byte(mem(A_OUT, CERT_SIZE), CERT_SIZE, 0xAA);
    const int32_t vec_rc = ioctlv(es, 0x1E, {{A_VEC1, 8}}, {{A_OUT, CERT_SIZE}});
    check("C7", short_rc == ES_EINVAL && short_untouched && vec_rc == ES_EINVAL,
          "short buffer -> " + std::to_string(short_rc) + ", 1 in/1 out -> " + std::to_string(vec_rc) +
              " (expected -1017 twice, buffer untouched)");

    // C8: the calls Skyward Sword issues (REL22-CERT logs: 0x20 0x1D 0x1B 0x16)
    {
        bool ok = ioctlv(es, 0x20, {}, {{A_OUT, 8}}) == 0 && be32(A_OUT) == 0x00010000u && be32(A_OUT + 4) == 0x534f5545u;
        put_be32(A_VEC1, 0x00010000);
        put_be32(A_VEC1 + 4, 0x534f5545);
        std::memset(mem(A_OUT, 0x40), 0xAA, 0x40);
        ok = ok && ioctlv(es, 0x1D, {{A_VEC1, 8}}, {{A_OUT, 0x1e}}) == 0 &&
             std::string(reinterpret_cast<const char*>(mem(A_OUT))) == "/title/00010000/534f5545/data";
        ok = ok && ioctlv(es, 0x1B, {{0, 0}}, {{A_OUT, 0xD8}}) == 0 && be32(A_OUT + 0x14) == 0x534f5545u;
        put_be32(A_VEC2, 0x12345678);
        ok = ok && ioctlv(es, 0x16, {{A_VEC1, 8}}, {{A_OUT, 0x10}, {A_VEC2, 4}}) == 0 && be32(A_VEC2) == 0u;
        check("C8", ok, "GetTitleId / GetTitleDir / DIGetTicketView / GetConsumption unchanged");
    }

    // C9: no device id / signing service
    {
        std::memset(mem(A_OUT, 0x40), 0xAA, 0x40);
        const int32_t id_rc = ioctlv(es, 0x07, {}, {{A_OUT, 4}});
        const int32_t sign_rc = ioctlv(es, 0x30, {{A_VEC1, 8}}, {{A_OUT, 0x3C}, {A_VEC2, CERT_SIZE}});
        check("C9", id_rc == IPC_EINVAL && sign_rc == IPC_EINVAL && all_byte(mem(A_OUT, 0x40), 0x40, 0xAA),
              "GetDeviceId 0x07 -> " + std::to_string(id_rc) + ", Sign 0x30 -> " + std::to_string(sign_rc) +
                  " (expected IPC_EINVAL, nothing written)");
    }
    g_kernel->close(*g_ctx, es);

    std::cout << "RESULT pass=" << g_pass << " fail=" << g_fail << std::endl;
    return g_fail ? 1 : 0;
}
