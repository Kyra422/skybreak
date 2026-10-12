#include "runtime/platform/platform.h"
#include "runtime/config.h"
#include "runtime/cpu_context.h"
#include "runtime/ss_boot.h"
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <thread>
#include "runtime/hw/hw.h"
#include "runtime/event_scheduler.h"
#include "runtime/os_thread.h"
#include "runtime/virtual_disc.h"

using namespace nwii::runtime;

namespace nwii::runtime {
    std::string read_guest_string(CPUContext &ctx, uint32_t addr, int max_len = 256);
    bool valid_callback(uint32_t cb);
}

#include "runtime/ios_device.h"
#include "runtime/ios_kernel.h"
#include "runtime/devices.h"
#include <iostream>
#include <vector>

extern "C" void run_game(nwii::runtime::CPUContext &ctx);

static uint32_t phys_to_virt(uint32_t addr) {
  if (addr == 0)
    return 0;
  if (addr >= 0x80000000 && addr < 0x94000000)
    return addr;
  if (addr >= 0x00800000 && addr < 0x04000000)
    return addr | 0x90000000;
  if (addr < 0x00800000)
    return addr | 0x80000000;
  if (addr >= 0x10000000 && addr < 0x14000000)
    return addr | 0x90000000;
  return addr;
}

std::string nwii::runtime::read_guest_string(CPUContext &ctx, uint32_t addr,
                                     int max_len) {
  std::string path;
  if (addr == 0) {
    std::cout << "[HLE IOS] read_guest_string: null address! pc=0x" << std::hex
              << ctx.pc << " lr=0x" << ctx.lr << std::dec << std::endl;
    return path;
  }
  uint32_t vaddr = phys_to_virt(addr);
  if (vaddr >= 0x80000000 && vaddr < 0x94000000) {
    for (int i = 0; i < max_len; ++i) {
      char c = ctx.mmu.read8(vaddr + i);
      if (c == '\0')
        break;
      if (c >= 32 && c <= 126)
        path += c;
    }
    if (path.empty()) {
      std::cout << "[HLE IOS] read_guest_string: string at 0x" << std::hex
                << addr << " (vaddr 0x" << vaddr
                << ") is empty (first char is 0x" << (int)ctx.mmu.read8(vaddr)
                << ") pc=0x" << ctx.pc << " lr=0x" << ctx.lr << std::dec
                << std::endl;
    }
  } else {
    std::cout << "[HLE IOS] read_guest_string: invalid translated address 0x"
              << std::hex << vaddr << " (orig: 0x" << addr << ") pc=0x"
              << ctx.pc << " lr=0x" << ctx.lr << std::dec << std::endl;
  }
  return path;
}

static constexpr int32_t IPC_EIO = -8;


static constexpr uint32_t IPC_POOL_BASE = 0x93EE0000;
static constexpr uint32_t IPC_SLOT_SIZE = 128; 
static constexpr uint32_t IPC_POOL_SLOTS = 64;
static uint32_t ipc_pool_next = 0; 




static uint32_t ios_guest_heap_ptr = 0x93E40000;
static uint32_t ios_ipc_heap_ptr = 0x93E00000;

static uint32_t ios_guest_alloc(CPUContext &ctx, uint32_t size, uint32_t align,
                                int heap_id = 0) {
  if (align < 32)
    align = 32;
  
  uint32_t &heap =
      (heap_id == 0 && size <= 128) ? ios_ipc_heap_ptr : ios_guest_heap_ptr;
  heap = (heap + align - 1) & ~(align - 1);
  uint32_t ptr = heap;
  heap += (size + align - 1) & ~(align - 1);
  for (uint32_t i = 0; i < size; ++i)
    ctx.mmu.write8(ptr + i, 0);
  return ptr;
}

static uint32_t ipc_pool_alloc(CPUContext &ctx) {
  uint32_t slot = ipc_pool_next;
  ipc_pool_next = (ipc_pool_next + 1) % IPC_POOL_SLOTS;
  uint32_t ptr = IPC_POOL_BASE + slot * IPC_SLOT_SIZE;
  
  for (uint32_t i = 0; i < IPC_SLOT_SIZE; ++i)
    ctx.mmu.write8(ptr + i, 0);
  return ptr;
}


bool nwii::runtime::valid_callback(uint32_t cb) {
  return cb != 0 && cb != 0xFFFFFFFFu && cb >= 0x80000000u && cb < 0x82000000u;
}

static std::shared_ptr<std::ifstream> g_disc_file;
static bool g_disc_is_wbfs = false;
static uint32_t g_wbfs_block_size = 0;
static std::vector<uint16_t> g_wbfs_bat;

static bool init_wbfs(const std::filesystem::path &path) {
  auto file = std::make_shared<std::ifstream>(path, std::ios::binary);
  if (!file->is_open())
    return false;

  char magic[4];
  file->read(magic, 4);
  if (magic[0] != 'W' || magic[1] != 'B' || magic[2] != 'F' ||
      magic[3] != 'S') {
    return false;
  }

  uint8_t shift;
  file->read(reinterpret_cast<char *>(&shift), 1);
  g_wbfs_block_size = 1u << (shift + 17);

  file->seekg(0, std::ios::end);
  size_t file_size = file->tellg();
  file->seekg(0x100, std::ios::beg);

  size_t num_entries = (file_size - 0x100) / 2;
  if (num_entries > 65536)
    num_entries = 65536;

  g_wbfs_bat.resize(num_entries);
  for (size_t i = 0; i < num_entries; ++i) {
    uint16_t entry;
    file->read(reinterpret_cast<char *>(&entry), 2);
    g_wbfs_bat[i] = ((entry >> 8) & 0xFF) | ((entry << 8) & 0xFF00);
  }

  g_disc_file = file;
  g_disc_is_wbfs = true;
  std::cout << "[HLE IOS] Opened WBFS: " << path
            << " block_size=" << g_wbfs_block_size << std::endl;
  return true;
}

static void init_iso(const std::filesystem::path &path) {
  g_disc_file = std::make_shared<std::ifstream>(path, std::ios::binary);
  if (g_disc_file->is_open()) {
    g_disc_is_wbfs = false;
    std::cout << "[HLE IOS] Opened ISO: " << path << std::endl;
  }
}

static std::shared_ptr<std::ifstream> get_disc_file() {
  if (!g_disc_file) {
    auto &cfg = Config::get();
    std::filesystem::path game_dir(cfg.game_dir);
    std::filesystem::path p;

    p = game_dir / "disc.iso";
    if (std::filesystem::exists(p)) {
      init_iso(p);
      return g_disc_file;
    }
    p = game_dir / "game.iso";
    if (std::filesystem::exists(p)) {
      init_iso(p);
      return g_disc_file;
    }
    p = game_dir.parent_path() / "game.iso";
    if (std::filesystem::exists(p)) {
      init_iso(p);
      return g_disc_file;
    }
    p = game_dir.parent_path() / "disc.iso";
    if (std::filesystem::exists(p)) {
      init_iso(p);
      return g_disc_file;
    }

    if (std::filesystem::exists(game_dir)) {
      for (const auto &entry : std::filesystem::directory_iterator(game_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".wbfs") {
          if (init_wbfs(entry.path()))
            return g_disc_file;
        }
      }
    }
    if (std::filesystem::exists(game_dir.parent_path())) {
      for (const auto &entry :
           std::filesystem::directory_iterator(game_dir.parent_path())) {
        if (entry.is_regular_file() && entry.path().extension() == ".wbfs") {
          if (init_wbfs(entry.path()))
            return g_disc_file;
        }
      }
    }
  }
  return g_disc_file;
}

static void read_disc_data(uint8_t *dst, uint64_t offset, uint32_t length) {
  
  auto &vd = VirtualDisc::get();
  if (!vd.valid())
    vd.init(Config::get().game_dir);
  if (vd.valid() && vd.read(offset, length, dst))
    return;

  auto file = get_disc_file();
  if (!file || !file->is_open()) {
    std::memset(dst, 0, length);
    return;
  }

  if (!g_disc_is_wbfs) {
    file->seekg(offset);
    file->read(reinterpret_cast<char *>(dst), length);
  } else {
    uint32_t done = 0;
    while (done < length) {
      uint64_t block_idx = (offset + done) / g_wbfs_block_size;
      uint32_t block_off = (offset + done) % g_wbfs_block_size;
      uint32_t chunk =
          std::min<uint32_t>(length - done, g_wbfs_block_size - block_off);

      if (block_idx < g_wbfs_bat.size() && g_wbfs_bat[block_idx] != 0) {
        uint64_t phys =
            (uint64_t)g_wbfs_bat[block_idx] * g_wbfs_block_size + block_off;
        file->seekg(phys);
        file->read(reinterpret_cast<char *>(dst + done), chunk);
      } else {
        std::memset(dst + done, 0, chunk);
      }
      done += chunk;
    }
  }
}

namespace nwii {
namespace runtime {

struct PendingCallback {
  uint32_t callback;
  uint32_t arg1;
  uint32_t arg2;
};

static std::vector<PendingCallback> g_pending_callbacks;



} 
} 

static void di_read_internal(nwii::runtime::MMU *mmu, uint32_t inbuf, uint32_t outbuf,
                             uint32_t callback, uint32_t userdata,
                             bool is_async, uint32_t out_size = 0) {
  
  uint32_t cmd = 0, transferSize = 0, addr = 0, length = 0;
  uint64_t offset = 0;

  uint32_t ipc_ptr = phys_to_virt(inbuf);

  if (ipc_ptr >= 0x80000000 && ipc_ptr < 0x94000000) {
    uint32_t raw_cmd = mmu->read32(ipc_ptr + 0x00);
    cmd = raw_cmd >> 24; 
    if (cmd == 1) {
      cmd = 0x12; 
    } else if (cmd == 0) {
      
      if (raw_cmd == 0x20)
        cmd = 0x20;
    }

    transferSize = mmu->read32(ipc_ptr + 0x04);
    addr = mmu->read32(ipc_ptr + 0x08);

    uint32_t val10 = mmu->read32(ipc_ptr + 0x10);
    uint32_t val14 = mmu->read32(ipc_ptr + 0x14);

    if (nwii::runtime::Config::get().platform ==
        nwii::runtime::Platform::GameCube) {
      offset = ((uint64_t)val14) << 2; 
    } else {
      
      offset = (((uint64_t)(val10 & 0xFFFF) << 32) | val14) << 2;
    }

    length = mmu->read32(ipc_ptr + 0x18);
  }
  
  if (addr == 0 && outbuf != 0) {
      addr = outbuf;
  }

  std::cout << "[HLE IOS] DI_Read" << (is_async ? "Async" : "") << ": inbuf=0x"
            << std::hex << inbuf << " cmd=0x" << cmd
            << " transferSize=" << std::dec << transferSize << " addr=0x"
            << std::hex << addr << " cb=0x" << callback << " offset=0x"
            << offset << " length=" << std::dec << length << " outbuf=0x" << std::hex << outbuf << std::endl;

  bool use_defaults =
      (cmd != 0x20 && cmd != 0x12 && cmd != 0x95 && cmd != 0x71 && cmd != 0x72 && cmd != 0x96 && cmd != 0xe3) || (offset > 0x1FFFFFFFFFFULL);

  if (use_defaults) {
    std::cout << "[HLE IOS] DI_Read: Using defaults (invalid structure)"
              << std::endl;
    cmd = 0x20;
    transferSize = 2;
    offset = 0;
    length = 0x440;
  }

  uint32_t buf_ptr = phys_to_virt(addr);
  uint32_t total_length = transferSize * 2048;
  if (length > 0 && length < total_length)
    total_length = length;
  if (total_length == 0 && length > 0)
    total_length = length;
  if (total_length == 0)
    total_length = 0x440;
  
  if (out_size > 0 && total_length > out_size)
    total_length = out_size;

  std::cout << "[HLE IOS] DI_Read final: offset=0x" << std::hex
            << (uint32_t)offset << " total_length=" << std::dec << total_length
            << " buf_ptr=0x" << std::hex << buf_ptr << std::dec << std::endl;

  auto file = get_disc_file();
  bool success = false;
  uint32_t bytes_read = 0;
  bool have_media = (file && file->is_open()) || VirtualDisc::get().valid();

  if (have_media && buf_ptr != 0 && total_length > 0) {
    std::vector<uint8_t> temp(total_length);
    read_disc_data(temp.data(), (uint32_t)offset, total_length);
    bytes_read = total_length;
    for (uint32_t i = 0; i < bytes_read; i++)
      mmu->write8(buf_ptr + i, temp[i]);
    std::cout << "[DI] Read " << bytes_read << " bytes" << std::endl;
    success = true;
  } else if (buf_ptr != 0) {
    if (offset == 0) {
      const std::string& gid = Config::get().game_id;
      for (uint32_t i = 0; i < total_length; i++) mmu->write8(buf_ptr + i, 0);
      for (int i = 0; i < (int)gid.size() && i < 4; i++)
        mmu->write8(buf_ptr + i, (uint8_t)gid[i]);
      mmu->write32(buf_ptr + 0x18, 0x5D1C9EA3);
      std::cout << "[DI] Faked disc header (" << gid << ")" << std::endl;
      success = true;
      bytes_read = total_length;
    } else {
      
      std::cout << "[DI] No disc file, zeroing out read buffer of size " << total_length << std::endl;
      for (uint32_t i = 0; i < total_length; i++) mmu->write8(buf_ptr + i, 0);
      success = true;
      bytes_read = total_length;
    }
  }
}

void di_read_internal_shared(nwii::runtime::MMU* mmu, uint32_t inbuf, uint32_t outbuf,
                              uint32_t callback, uint32_t userdata, bool is_async,
                              uint32_t out_size) {
    di_read_internal(mmu, inbuf, outbuf, callback, userdata, is_async, out_size);
}

extern "C" {
void IOS_Open(CPUContext &ctx) {
    nwii::runtime::platform::IPlatform::get().ios_open(ctx);
}

void IOS_OpenAsync(CPUContext &ctx) {
    nwii::runtime::platform::IPlatform::get().ios_open_async(ctx);
}

void IOS_Close(CPUContext &ctx) {
    nwii::runtime::platform::IPlatform::get().ios_close(ctx);
}

void IOS_Read(CPUContext &ctx) {
    nwii::runtime::platform::IPlatform::get().ios_read(ctx);
}

void IOS_Write(CPUContext &ctx) {
    nwii::runtime::platform::IPlatform::get().ios_write(ctx);
}

void IOS_Ioctl(CPUContext &ctx) {
    nwii::runtime::platform::IPlatform::get().ios_ioctl(ctx);
}

void IOS_Ioctlv(CPUContext &ctx) {
    nwii::runtime::platform::IPlatform::get().ios_ioctlv(ctx);
}

void iosAlloc(CPUContext &ctx) {
  uint32_t heap_id = ctx.gpr[3];
  uint32_t size = ctx.gpr[4];
  uint32_t align = ctx.gpr[5];
  uint32_t ptr = ios_guest_alloc(ctx, size, align, heap_id);
  std::cout << "[HLE IOS] iosAlloc: heap=" << heap_id << " size=" << size
            << " align=" << align << " -> ptr=0x" << std::hex << ptr << " LR=0x" << ctx.lr << std::dec
            << std::endl;
  ctx.gpr[3] = ptr;
  ctx.pc = ctx.lr;
}


void iosFree(CPUContext &ctx) {
  uint32_t heap_id = ctx.gpr[3];
  uint32_t ptr = ctx.gpr[4];
  std::cout << "[HLE IOS] iosFree: heap=" << heap_id << " ptr=0x" << std::hex
            << ptr << " LR=0x" << ctx.lr << std::dec << std::endl;
  ctx.gpr[3] = 0; 
  ctx.pc = ctx.lr;
}

void IOS_IoctlAsync(CPUContext &ctx) {
    nwii::runtime::platform::IPlatform::get().ios_ioctl_async(ctx);
}

void IOS_IoctlvAsync(CPUContext &ctx) {
    nwii::runtime::platform::IPlatform::get().ios_ioctlv_async(ctx);
}

void IOS_Seek(CPUContext &ctx) {
  if (Config::get().platform == Platform::GameCube) {
    ctx.gpr[3] = -1;
    ctx.pc = ctx.lr;
    return;
  }
  int32_t fd = (int32_t)ctx.gpr[3];
  int32_t offset = (int32_t)ctx.gpr[4];
  int32_t whence = (int32_t)ctx.gpr[5];

  int32_t result = IOSKernel::get().seek(ctx, fd, offset, whence);
  ctx.gpr[3] = result;
  ctx.pc = ctx.lr;
}

void IOS_CloseAsync(CPUContext &ctx) {
    nwii::runtime::platform::IPlatform::get().ios_close_async(ctx);
}

void IOS_ReadAsync(CPUContext &ctx) {
    nwii::runtime::platform::IPlatform::get().ios_read_async(ctx);
}

void IOS_WriteAsync(CPUContext &ctx) {
    nwii::runtime::platform::IPlatform::get().ios_write_async(ctx);
}

void IOS_SeekAsync(CPUContext &ctx) {
  if (Config::get().platform == Platform::GameCube) {
    ctx.gpr[3] = -1;
    ctx.pc = ctx.lr;
    return;
  }
  int32_t fd = (int32_t)ctx.gpr[3];
  int32_t offset = (int32_t)ctx.gpr[4];
  int32_t whence = (int32_t)ctx.gpr[5];
  uint32_t callback = ctx.gpr[6];
  uint32_t userdata = ctx.gpr[7];

  int32_t result = IOSKernel::get().seek(ctx, fd, offset, whence);

  if (valid_callback(callback)) {
    ctx.queue_callback(callback, result, userdata);
  }

  ctx.gpr[3] = IPC_OK;
  ctx.pc = ctx.lr;
}

} 

namespace nwii {
namespace runtime {
extern MMU *g_mmu;
}
} 

extern "C" int32_t handle_ios_ipc(CPUContext& ctx, uint32_t request_addr) {
  if (!nwii::runtime::g_mmu)
    return 0;

  

  uint32_t virt_addr = request_addr;
  if (virt_addr < 0x01800000) {
    virt_addr |= 0x80000000;
  } else if (virt_addr >= 0x10000000 && virt_addr < 0x14000000) {
    virt_addr = (virt_addr & 0x03FFFFFF) | 0x90000000;
  }

  // IOS IPC request layout (Dolphin IOS::HLE):
  
  uint32_t cmd = nwii::runtime::g_mmu->read32(virt_addr);
  int32_t result = IPC_OK;

  if (cmd == 1) { 
    
    uint32_t path_addr = nwii::runtime::g_mmu->read32(virt_addr + 0x0C);
    uint32_t mode = nwii::runtime::g_mmu->read32(virt_addr + 0x10);
    
    if (path_addr < 0x01800000)
      path_addr |= 0x80000000;
    else if (path_addr >= 0x10000000 && path_addr < 0x14000000)
      path_addr = (path_addr & 0x03FFFFFF) | 0x90000000;

    std::string path;
    for (int i = 0; i < 64; ++i) {
      char c = (char)ctx.mmu.read8(path_addr + i);
      if (c == '\0') break;
      path += c;
    }
    result = IOSKernel::get().open(ctx, path, mode);
    std::cout << "[HW IPC] Open path='" << path << "' -> fd=" << result << "\n";
  } else if (cmd == 2) { 
    uint32_t fd = ctx.mmu.read32(virt_addr + 0x08);
    result = IOSKernel::get().close(ctx, fd);
  } else if (cmd == 3) { 
    uint32_t fd = ctx.mmu.read32(virt_addr + 0x08);
    uint32_t buf = ctx.mmu.read32(virt_addr + 0x0C);
    uint32_t len = ctx.mmu.read32(virt_addr + 0x10);
    result = IOSKernel::get().read(ctx, fd, buf, len);
  } else if (cmd == 4) { 
    uint32_t fd = ctx.mmu.read32(virt_addr + 0x08);
    uint32_t buf = ctx.mmu.read32(virt_addr + 0x0C);
    uint32_t len = ctx.mmu.read32(virt_addr + 0x10);
    result = IOSKernel::get().write(ctx, fd, buf, len);
  } else if (cmd == 5) { 
    uint32_t fd = ctx.mmu.read32(virt_addr + 0x08);
    int32_t offset = (int32_t)ctx.mmu.read32(virt_addr + 0x0C);
    int32_t whence = (int32_t)ctx.mmu.read32(virt_addr + 0x10);
    result = IOSKernel::get().seek(ctx, fd, offset, whence);
  } else if (cmd == 6) { 
    uint32_t fd = ctx.mmu.read32(virt_addr + 0x08);
    uint32_t ioctl_cmd = ctx.mmu.read32(virt_addr + 0x0C);
    uint32_t in_buf = ctx.mmu.read32(virt_addr + 0x10);
    uint32_t in_size = ctx.mmu.read32(virt_addr + 0x14);
    uint32_t out_buf = ctx.mmu.read32(virt_addr + 0x18);
    uint32_t out_size = ctx.mmu.read32(virt_addr + 0x1C);

    std::cout << "[HW IPC] Ioctl fd=" << fd << " cmd=0x" << std::hex << ioctl_cmd << std::dec << "\n";

    IpcRequest req{};
    req.fd = fd;
    req.req_addr = virt_addr;
    req.ioctl_cmd = ioctl_cmd;
    req.in_buf = in_buf;
    req.in_size = in_size;
    req.out_buf = out_buf;
    req.out_size = out_size;
    result = IOSKernel::get().ioctl(ctx, req);
  } else if (cmd == 7) { 
    uint32_t fd = ctx.mmu.read32(virt_addr + 0x08);
    uint32_t ioctl_cmd = ctx.mmu.read32(virt_addr + 0x0C);
    uint32_t arg_in = ctx.mmu.read32(virt_addr + 0x10);
    uint32_t arg_out = ctx.mmu.read32(virt_addr + 0x14);
    uint32_t arg_array = ctx.mmu.read32(virt_addr + 0x18);

    IpcRequest req{};
    req.cmd = cmd;
    req.fd = fd;
    req.req_addr = virt_addr;
    req.ioctl_cmd = ioctl_cmd;
    req.arg_cnt_in = arg_in;
    req.arg_cnt_out = arg_out;

    std::cout << "[HW IPC] Parsed IOCTLV fd=" << fd << " cmd=" << std::hex << ioctl_cmd << std::dec 
              << " in=" << arg_in << " out=" << arg_out << "\n";

    for (uint32_t i = 0; i < arg_in + arg_out; i++) {
        IoctlvVector vec;
        vec.addr = ctx.mmu.read32(arg_array + i * 8);
        vec.len = ctx.mmu.read32(arg_array + i * 8 + 4);
        req.ioctlv_vecs.push_back(vec);
    }

    result = IOSKernel::get().ioctlv(ctx, req);
  }

  std::cout << "[HW IPC] cmd=" << cmd << " virt=0x" << std::hex << virt_addr
            << " -> result=" << (int32_t)result << std::dec << std::endl;

  if (result != -0x70000001) {
      nwii::runtime::g_mmu->write32(virt_addr + 4, (uint32_t)(int32_t)result);
      // PERF3 (c): reply block as IOS writes it (Dolphin Kernel::EnqueueIPCReply):
      // the answered command goes into the fd field (+8) and the command word
      // becomes IPC_REPLY (8). SDK IpcReplyHandler (SOUE01 0x80425880) switches
      // on +8 to pick the cache work and the physical->virtual fix-ups; with
      // the fd left there, fd 3/6/7 replies ran the read/ioctl/ioctlv branches
      // on unrelated fields (e.g. write(fd 7, 0xFBE0 bytes) -> 0xFBE0+ garbage
      // "vectors": DCInvalidateRange loops worth ~1.5e13 instructions, and the
      // handler's stwx rewrote guest memory). NWII_IPC_REPLY_CMD=0: old layout.
      static const bool reply_cmd = [] {
        const char *e = std::getenv("NWII_IPC_REPLY_CMD");
        return !(e && e[0] == '0');
      }();
      if (reply_cmd && cmd != 8) {
        nwii::runtime::g_mmu->write32(virt_addr + 8, cmd);
        nwii::runtime::g_mmu->write32(virt_addr, 8);
      }
  }
  return result;
}

namespace nwii {
namespace runtime {

void init_ipc_client(CPUContext &ctx) {
  IOSKernel::get().init();
  nwii::runtime::devices::register_all();
}

// PowerPC `sc`. On the Wii the PPC never talks to IOS through a system call:
// IOS is reached through the IPC registers (HW IPC path above). The only `sc`
// sites in a retail SDK DOL are PPCSync, DCFlushRange and DCStoreRange
// (SOUE01: 0x803A1D10, 0x803A45B8, 0x803A45E8), where the OS's system-call
// vector (__OSSystemCallVectorStart) just toggles HID0[ABE] around a `sync`
// and returns with rfi. So `sc` is a no-op memory barrier here; it touches no
// GPR. (It used to dispatch on r0 to IOS_* entry points, which turned a
// DCFlushRange with a stale r0 == 0x6C into a bogus IOS_IoctlvAsync whose
// "fd" was the flushed address, e.g. fd=0x80EE9800, and clobbered r3.)
// The exception return is still an interrupt opportunity, so poll there.
bool handle_syscall(CPUContext &ctx) {
  const uint32_t resume_pc = ctx.pc;
  if (process_pending_callbacks(ctx))
    return true;
  ctx.pc = resume_pc;
  return false;
}

} 
} 

#include "runtime/cpu_context.h"
#include "runtime/profiler.h"
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#include <csetjmp>
#include <cstdint>

namespace nwii::runtime {



bool g_ipc_reply_pending = false;

int g_ipc_interrupt_delay = 0;

// ---------------------------------------------------------------------------
// M0.3 poll-path overhead. process_pending_callbacks runs on every recompiled
// back-edge (20-50M times/s in a boot-title probe, most of it in the
// SelectThread idle loop). Every diagnostic switch it consults is read from
// the environment once, at static initialisation, into namespace-scope
// constants: no std::getenv and no function-local static (MSVC guards those
// with a thread-local epoch check on every call) on the poll path. Periodic
// prints are only emitted when NWII_SAMPLE is set.
// ---------------------------------------------------------------------------
namespace {
struct PollDiagFlags {
  bool sample = false;         // NWII_SAMPLE: sampled diagnostics + periodic poll prints
  uint32_t loop_trace_pc = 0;  // NWII_LOOPTRACE=<hex pc>: print every 200000th poll there
  uint32_t resched_addr = 0;   // NWII_RESCHED=<hex addr>: legacy reschedule entry
};

PollDiagFlags load_poll_diag() {
  PollDiagFlags f;
  f.sample = std::getenv("NWII_SAMPLE") != nullptr;
  if (const char *e = std::getenv("NWII_LOOPTRACE"))
    f.loop_trace_pc = (uint32_t)std::strtoul(e, nullptr, 16);
  if (const char *e = std::getenv("NWII_RESCHED"))
    f.resched_addr = (uint32_t)std::strtoul(e, nullptr, 16);
  return f;
}

// Initialised before main(); nothing polls during static initialisation.
const PollDiagFlags g_poll_diag = load_poll_diag();

inline const PollDiagFlags &poll_diag() { return g_poll_diag; }

// The AI sample counter is driven by the host clock and every AI MMIO access
// settles it first (hw_ai.cpp read_ai/write_ai), so the guest-visible AISCNT
// value does not depend on how often the poll runs. The poll only has to
// notice an AISCNT==AIIT crossing (AIINT). advance_counter() detects every
// crossing inside the elapsed interval, so polling it once per 1024 timebase
// ticks only delays AIINT delivery. The timebase is inst_count by default, so
// 1024 ticks = 1024 guest instructions = 6-10 us of host time at the measured
// 100-165M inst/s (17 us with NWII_WALLTB); one 32 kHz sample is 31 us.
constexpr uint64_t kAiPollTicks = 1024;
uint64_t g_ai_last_poll_tb = 0;
bool g_ai_polled_once = false;

// Resolved once at static initialisation (EventScheduler::get() is a
// function-local singleton, so constructing it here is order-safe).
nwii::runtime::EventScheduler &g_poll_sched = nwii::runtime::EventScheduler::get();

inline nwii::runtime::EventScheduler &poll_scheduler() { return g_poll_sched; }

// Statistics for the [POLLSTAT] line (NWII_SAMPLE only).
uint64_t g_poll_calls = 0;
uint64_t g_poll_ai_updates = 0;

} // namespace

uint64_t poll_call_count() { return g_poll_calls; } // PERF2 profiler

namespace {
void poll_stat_report(CPUContext &ctx) {
  using clock = std::chrono::steady_clock;
  static const clock::time_point start = clock::now();
  static clock::time_point last = start;
  static uint64_t last_inst = 0, last_polls = 0;
  const clock::time_point now = clock::now();
  if (now - last < std::chrono::seconds(5))
    return;
  const double dt = std::chrono::duration<double>(now - last).count();
  const uint64_t t_ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
  std::cout << std::dec << "[POLLSTAT] t_ms=" << t_ms << " inst=" << ctx.inst_count
            << " polls=" << g_poll_calls << " ai_updates=" << g_poll_ai_updates
            << " inst_per_s=" << (uint64_t)((ctx.inst_count - last_inst) / dt)
            << " polls_per_s=" << (uint64_t)((g_poll_calls - last_polls) / dt)
            << "\n";
  last = now;
  last_inst = ctx.inst_count;
  last_polls = g_poll_calls;
}
} // namespace


static constexpr uint32_t CALLBACK_STACK_TOP_DEFAULT = 0x816F0000;

static inline uint32_t get_callback_stack_top() {
    uint32_t v = Config::get().game_profile.callback_stack_top;
    return v ? v : CALLBACK_STACK_TOP_DEFAULT;
}

void hle_drive_thread_queue(CPUContext& ctx) {
    
}



static void hle_save_context_to_guest(CPUContext &ctx, uint32_t c) {
  if (poll_diag().sample) {
      static int n = 0;
      if (n++ < 40)
          std::cout << "[CtxSave] #" << n << " ctx=0x" << std::hex << c
                    << " srr0<=0x" << ctx.pc << " msr=0x" << ctx.msr
                    << " lr=0x" << ctx.lr << std::dec << "\n";
  }
  for (int i = 0; i < 32; i++)
    ctx.mmu.write32(c + i * 4, ctx.gpr[i]);

  

  // This helper saves the FPU as well as the interrupted integer context.
  // Original OSLoadFPUContext tests FP_SAVED (bit 0) before loading its lanes.
  ctx.mmu.write16(c + 0x1A2, ctx.mmu.read16(c + 0x1A2) | 0x0003);
  uint32_t cr_val = 0;
  for (int i = 0; i < 8; i++) {
    cr_val |= (ctx.cr[i].lt ? 8u : 0) << (28 - i * 4);
    cr_val |= (ctx.cr[i].gt ? 4u : 0) << (28 - i * 4);
    cr_val |= (ctx.cr[i].eq ? 2u : 0) << (28 - i * 4);
    cr_val |= (ctx.cr[i].so ? 1u : 0) << (28 - i * 4);
  }
  ctx.mmu.write32(c + 0x80, cr_val);
  ctx.mmu.write32(c + 0x84, ctx.lr);
  ctx.mmu.write32(c + 0x88, ctx.ctr);
  ctx.mmu.write32(c + 0x8C, ctx.xer);
  for (int i = 0; i < 32; i++) {
    uint64_t d;
    std::memcpy(&d, &ctx.fpr[i], 8);
    ctx.mmu.write64(c + 0x90 + i * 8, d);
  }
  ctx.mmu.write32(c + 0x194, ctx.fpscr);
  ctx.mmu.write32(c + 0x198, ctx.pc);  
  ctx.mmu.write32(c + 0x19C, ctx.msr); 
  for (int i = 0; i < 8; i++)
    ctx.mmu.write32(c + 0x1A4 + i * 4, ctx.gqr[i]);
  for (int i = 0; i < 32; i++) {
    // OSContext::psfs is storage for psq_st W=0,I=0: two big-endian
    // float32 lanes, not a float64 PS1 value. The double PS0 is above at 0x90.
    ctx.mmu.write_f32(c + 0x1C8 + i * 8, static_cast<float>(ctx.fpr[i]));
    ctx.mmu.write_f32(c + 0x1CC + i * 8, static_cast<float>(ctx.ps1[i]));
  }
}


static bool hle_load_context_from_guest(CPUContext &ctx) {
  uint32_t c = ctx.mmu.read32(0x800000D4);
  if (c == 0)
    return false;
  for (int i = 0; i < 32; i++)
    ctx.gpr[i] = ctx.mmu.read32(c + i * 4);
  uint32_t cr_val = ctx.mmu.read32(c + 0x80);
  for (int i = 0; i < 8; i++) {
    uint32_t nib = (cr_val >> (28 - i * 4)) & 0xF;
    ctx.cr[i].lt = (nib & 8) != 0;
    ctx.cr[i].gt = (nib & 4) != 0;
    ctx.cr[i].eq = (nib & 2) != 0;
    ctx.cr[i].so = (nib & 1) != 0;
  }
  ctx.lr = ctx.mmu.read32(c + 0x84);
  ctx.ctr = ctx.mmu.read32(c + 0x88);
  ctx.xer = ctx.mmu.read32(c + 0x8C);
  for (int i = 0; i < 32; i++) {
    uint64_t d = ctx.mmu.read64(c + 0x90 + i * 8);
    std::memcpy(&ctx.fpr[i], &d, 8);
  }
  ctx.fpscr = ctx.mmu.read32(c + 0x194);
  for (int i = 0; i < 8; i++)
    ctx.gqr[i] = ctx.mmu.read32(c + 0x1A4 + i * 4);
  for (int i = 0; i < 32; i++) {
    // Original OSLoadFPUContext loads the paired slot, then replaces PS0
    // with its full double from 0x90. Read the remaining float32 PS1 lane.
    ctx.ps1[i] = ctx.mmu.read_f32(c + 0x1CC + i * 8);
  }
  ctx.pc = ctx.mmu.read32(c + 0x198);
  ctx.msr = ctx.mmu.read32(c + 0x19C);

  // GEN2 (M1.11 FPU ownership): the eager load above leaves c's FPU state in
  // the register file, so c is now the FPU owner. OSSetCurrentContext and
  // OSSwitchFPUContext (OSContext.c) treat __OSFPUContext (0x800000D8) as
  // "whose FPRs are live"; without this, a resume of another context than the
  // previous owner (the 0xFFFFFFFC path rewrites D4 to the current thread)
  // would later save c's live FPRs into the old owner and skip the reload when
  // that owner runs again. Only while the FP Unavailable model is active, so
  // NWII_FP_UNAVAILABLE=0 / an unguarded export keep the old behaviour.
  if (g_fp_unavailable_emulation && !g_env_fp_unavailable_off &&
      CPUContext::fp_guest_context(c) && ctx.fp_switch_handler_installed())
    ctx.mmu.write32(CPUContext::kOsFpuContext, c);



  
  ctx.mmu.write16(c + 0x1A2, ctx.mmu.read16(c + 0x1A2) & ~0x0002);
  return true;
}

int g_trace_after_dec = 0;

#if defined(_MSC_VER)
#define NWII_POLL_INLINE __forceinline
#define NWII_POLL_NOINLINE __declspec(noinline)
#define NWII_RETURN_ADDRESS() ((uintptr_t)_ReturnAddress())
#else
#define NWII_POLL_INLINE inline __attribute__((always_inline))
#define NWII_POLL_NOINLINE __attribute__((noinline))
#define NWII_RETURN_ADDRESS() ((uintptr_t)__builtin_return_address(0))
#endif

// The poll proper. process_pending_callbacks (below) is a thin wrapper that
// adds the PERF2 dispatcher shortcut; this body is unchanged.
static NWII_POLL_INLINE bool poll_body(CPUContext &ctx) {
  const PollDiagFlags &diag = poll_diag();
  ++g_poll_calls;
  if (ctx.pc == 0x804B75A4 && Config::get().game_id == "SOUE01") {
      SS_ExceptionWaitFailure(ctx);
  }
  if (ctx.pc == 0x802F004C && ctx.cr[0].eq == false &&
      Config::get().game_id == "SOUE01") {
      SS_DvdLoadFailure(ctx);
  }


  if (diag.sample) {
      static uint32_t last_ee = 0, edges = 0;
      uint32_t ee = ctx.msr & 0x8000;
      if (ee && !last_ee && edges < 8)
          std::cout << "[EEup] #" << ++edges << " pc=0x" << std::hex << ctx.pc
                    << " lr=0x" << ctx.lr << std::dec << "\n";
      last_ee = ee;
  }

  
  if (g_trace_after_dec > 0) {
      static int skip = 20000; 
      if (skip > 0) { skip--; }
      else {
          g_trace_after_dec--;
          std::cout << "[T] 0x" << std::hex << ctx.pc << " lr=0x" << ctx.lr
                    << " r1=0x" << ctx.gpr[1] << " r3=0x" << ctx.gpr[3]
                    << " r29=0x" << ctx.gpr[29] << " sr0=0x" << ctx.srr0
                    << std::dec << "\n";
      }
  }

  {
    const uint32_t trace_pc = diag.loop_trace_pc;
    static uint64_t hits = 0;
    if (trace_pc && ctx.pc == trace_pc) {
      if ((hits++ % 200000) == 0)
        std::cout << "[Loop 0x" << std::hex << trace_pc << "] #" << std::dec
                  << hits << " r3=0x" << std::hex << ctx.gpr[3] << " r4=0x"
                  << ctx.gpr[4] << " r5=0x" << ctx.gpr[5] << " r30=0x"
                  << ctx.gpr[30] << " r31=0x" << ctx.gpr[31] << " lr=0x"
                  << ctx.lr << std::dec << "\n";
    }
  }

  if (diag.sample) {
    static uint64_t n = 0;
    if ((++n % 400000) == 0)
      std::cout << "[Sample] pc=0x" << std::hex << ctx.pc << " lr=0x" << ctx.lr
                << " r3=0x" << ctx.gpr[3] << " r4=0x" << ctx.gpr[4]
                << " ctr=0x" << ctx.ctr << " r27=0x" << ctx.gpr[27]
                << " r28=0x" << ctx.gpr[28]
                << " osCtx=0x" << ctx.mmu.read32(0x800000D4)
                << std::dec << "\n";
    if ((g_poll_calls & 0xFFFFF) == 0)
      poll_stat_report(ctx);
  }

  

  
  if (ctx.pc == 0xFFFFFFFC && !ctx.in_callback) {

    

    
    
    const uint32_t g_resched_addr = diag.resched_addr;
    if (ctx.ext_resched_pending && g_resched_addr) {
      ctx.ext_resched_pending = false;
      if (diag.sample) {
        static int n = 0;
        if (n++ < 12) {
          uint32_t cur = ctx.mmu.read32(0x800000E4);
          std::cout << "[Resched] cur=0x" << std::hex << cur
                    << " prio=" << std::dec << ctx.mmu.read32(cur + 0x2D0)
                    << " r1=0x" << std::hex << ctx.gpr[1] << std::dec << "\n";
        }
      }
      ctx.gpr[3] = 0;
      ctx.lr = 0xFFFFFFFC;
      ctx.pc = g_resched_addr;
      return true;
    }
    ctx.ext_resched_pending = false;
    if (ctx.dispatch_saved_ctx) {
      uint32_t current_thread = ctx.mmu.read32(0x800000E4);
      if (current_thread != 0 && current_thread != ctx.dispatch_saved_ctx) {
        ctx.mmu.write32(0x800000D4, current_thread);
      } else {
        ctx.mmu.write32(0x800000D4, ctx.dispatch_saved_ctx);
      }
      ctx.dispatch_saved_ctx = 0;
    }
    if (hle_load_context_from_guest(ctx)) {

      

      
      ctx.dec_written_tb = ctx.read_timebase();
      static uint32_t restore_count = 0;
      restore_count++;
      // Periodic print only with NWII_SAMPLE; a restore to pc 0 is always logged.
      if (ctx.pc == 0 || (diag.sample && (restore_count % 64) == 1)) {
          std::cout << "[HLE PI] Context restore #" << std::dec << restore_count
                    << " pc=0x" << std::hex << ctx.pc << " r1=0x" << ctx.gpr[1]
                    << " msr=0x" << ctx.msr << std::dec << "\n";
      }
      return true;
    }
  }

  // Periodic heartbeat/thread dump: diagnostics only (NWII_SAMPLE).
  if (diag.sample && (ctx.inst_count % 20000000) == 0) {
      std::cout << "[Heartbeat] PC: 0x" << std::hex << ctx.pc << " LR: 0x" << ctx.lr
                << " intmr=0x" << nwii::runtime::hw::pi_intmr.load()
                << " intsr=0x" << nwii::runtime::hw::pi_intsr.load()
                << " msr=0x" << ctx.msr
                << " dec_h=0x" << ctx.mmu.read32(0x80003020) << "\n";
      {
          std::cout << "  [ExcTable]";
          for (int i = 0; i < 16; i++) {
              uint32_t h = ctx.mmu.read32(0x80003000 + i * 4);
              if (h) std::cout << " " << i << ":0x" << std::hex << h << std::dec;
          }
          std::cout << "\n";
      }

      
      
      auto dump_thread = [&](uint32_t th, const char *tag) {
          uint16_t state = ctx.mmu.read16(th + 0x2C8);
          uint32_t prio = ctx.mmu.read32(th + 0x2D0);
          uint32_t wqueue = ctx.mmu.read32(th + 0x2DC);
          uint32_t srr0 = ctx.mmu.read32(th + 0x198);
          uint32_t slr = ctx.mmu.read32(th + 0x84);
          std::cout << "  [" << tag << "] 0x" << std::hex << th << " state=" << state
                    << " prio=" << std::dec << prio << " waitq=0x" << std::hex
                    << wqueue << " srr0=0x" << srr0 << " lr=0x" << slr;
          
          uint32_t sp = ctx.mmu.read32(th + 4);
          for (int f = 0; f < 6 && sp != 0 && sp < 0x94000000; ++f) {
              uint32_t ret = ctx.mmu.read32(sp + 4);
              if (ret) std::cout << " <-0x" << std::hex << ret;
              sp = ctx.mmu.read32(sp);
          }
          std::cout << std::dec << "\n";
      };
      
      {
          static uint32_t peek_addr = 0, peek_words = 8;
          static bool peek_parsed = false;
          if (!peek_parsed) {
              peek_parsed = true;
              if (const char *env = std::getenv("NWII_PEEK")) {
                  peek_addr = (uint32_t)std::strtoul(env, nullptr, 16);
                  if (const char *c = std::strchr(env, ','))
                      peek_words = (uint32_t)std::strtoul(c + 1, nullptr, 10);
              }
          }
          if (peek_addr) {
              std::cout << "  [Peek 0x" << std::hex << peek_addr << "]";
              for (uint32_t i = 0; i < peek_words; i++)
                  std::cout << " " << ctx.mmu.read32(peek_addr + i * 4);
              std::cout << std::dec << "\n";
          }
      }
      uint32_t cur = ctx.mmu.read32(0x800000E4);
      if (cur) dump_thread(cur, "CurThread");
      uint32_t th = ctx.mmu.read32(0x800000DC);
      int guard = 0;
      while (th != 0 && guard++ < 16) {
          if (th != cur) dump_thread(th, "Thread");
          th = ctx.mmu.read32(th + 0x2FC);
      }
  }

  
  
  if (ctx.pc == 0) {
      static uint64_t idle_hits = 0;
      if (++idle_hits == 200000) {
          std::cout << "[IDLE] no runnable thread. intmr=0x" << std::hex
                    << nwii::runtime::hw::pi_intmr << " intsr=0x"
                    << nwii::runtime::hw::pi_intsr << " dec_armed="
                    << std::dec << ctx.dec_irq_pending << "\n";
          uint32_t cur = ctx.mmu.read32(0x800000E4);
          uint32_t th = ctx.mmu.read32(0x800000DC);
          int guard = 0;
          while (th != 0 && guard++ < 24) {
              uint16_t state = ctx.mmu.read16(th + 0x2C8);
              uint32_t prio = ctx.mmu.read32(th + 0x2D0);
              uint32_t wq = ctx.mmu.read32(th + 0x2DC);
              uint32_t srr0 = ctx.mmu.read32(th + 0x198);
              std::cout << "  [IdleThread] 0x" << std::hex << th
                        << (th == cur ? "*" : " ") << " state=" << std::dec
                        << state << " prio=" << prio << " waitq=0x" << std::hex
                        << wq << " srr0=0x" << srr0 << std::dec << "\n";
              th = ctx.mmu.read32(th + 0x2FC);
          }
      }
  }

  if (g_ipc_interrupt_delay > 0) {
      g_ipc_interrupt_delay--;
      if (g_ipc_interrupt_delay == 0) {
          nwii::runtime::hw::ipc_update_interrupts();
          if (diag.sample)
              std::cout << "[IPC] irq raised intmr=0x" << std::hex
                        << nwii::runtime::hw::pi_intmr << " msr=0x" << ctx.msr
                        << " pc=0x" << ctx.pc << std::dec << "\n";
      }
  }

  // di_tick() only counts down g_di_interrupt_delay (a poll-count delay), so
  // skipping the call while it is 0 is exact.
  if (nwii::runtime::hw::g_di_interrupt_delay > 0)
      nwii::runtime::hw::di_tick();
  // AI: settle the wall-clock sample counter at most once per kAiPollTicks of
  // guest timebase (see kAiPollTicks). pc==0 is the host-side idle path, where
  // the timebase does not advance, so it always updates.
  {
      const uint64_t tb = ctx.read_timebase();
      if (!g_ai_polled_once || tb - g_ai_last_poll_tb >= kAiPollTicks ||
          ctx.pc == 0) {
          g_ai_polled_once = true;
          g_ai_last_poll_tb = tb;
          ++g_poll_ai_updates;
          nwii::runtime::hw::ai_update();
      }
  }

  if (ctx.vblank_pending) {
      if (!ctx.in_callback && (ctx.msr & 0x8000)) {
          ctx.vblank_pending = false;
          nwii::runtime::prof::bump(nwii::runtime::prof::kViHost);
          nwii::runtime::hw::vi_trigger_interrupt();

          
          
          hle_drive_thread_queue(ctx);
      }
  }

  if (ctx.in_callback && ctx.pc == 0xFFFFFFFC) {
    
    ctx.in_callback = false;
  }

  if (ctx.in_callback) {
    if (diag.sample && (nwii::runtime::hw::pi_intsr & 0x4000)) {
        static uint64_t n = 0;
        if ((n++ % 100000) == 0)
            std::cout << "[IPC] irq pending but in_callback pc=0x" << std::hex
                      << ctx.pc << " lr=0x" << ctx.lr << " depth="
                      << std::dec << ctx.callback_depth << "\n";
    }
    return false;
  }

  // Periodically raise VI (PI_INTSR bit 8 = 0x100, Dolphin INT_CAUSE_VI)

  

  

  

  

  
  
  {
      static bool vi_registered = false;
      if (!vi_registered) {
          vi_registered = true;

          
          uint64_t vi_period = ctx.tb_freq / 60;
          if (vi_period == 0) vi_period = 500000;
          nwii::runtime::EventScheduler::get().schedule_recurring(
              vi_period, [](CPUContext& c, uint64_t) {

                  
                  
                  if ((nwii::runtime::hw::pi_intmr & 0x100) && !c.in_callback &&
                      !(nwii::runtime::hw::pi_intsr & 0x100) &&
                      (c.dispatch_saved_ctx == 0 || c.mmu.read32(0x800000E4) == 0)) {
                      nwii::runtime::prof::bump(nwii::runtime::prof::kViVirtual);
                      nwii::runtime::hw::vi_trigger_interrupt();
                  }
              });
      }
      // Still advanced on every poll: MMIO handlers schedule DI/AR/DSP
      // completions relative to the scheduler's m_now.
      poll_scheduler().advance(ctx, ctx.read_timebase());
  }


  if (ctx.dec_expired() && (ctx.msr & 0x8000) && !ctx.in_callback) {
      ctx.dec_irq_pending = false;
      if (diag.sample) {
          static uint32_t df = 0;
          if (df++ < 12)
              std::cout << "[DECfire] #" << df << " pc=0x" << std::hex << ctx.pc
                        << " lr=0x" << ctx.lr << " inst=" << std::dec
                        << ctx.inst_count << "\n";
          extern int g_trace_after_dec;
          if (df == 1) g_trace_after_dec = 300;
      }

      // (CAMERA track) The former "[HLE DEC] force-expired alarm" heuristic was removed here. After 64
      // consecutive DEC interrupts at one pc (routine in the SelectThread idle loop 0x803ACB68 with
      // the Dolphin timing model) it scanned the 2 KiB below r13 for "alarm-like" words and overwrote
      // them with the timebase, and dropped that DEC. Below r13 (0x80579440) lies .sdata2, so it
      // corrupted six float constants at boot: d_camera_game 0.25/1.0 (0x80579254/58; the game camera's
      // follow rate became 0 -> it never followed Link), d_camera_event 50.0/4.0 (0x80579444/48; the
      // fov-0 event camera) and d_sword_swing_effect_mgr -1.0/0.1 (0x8057921C/20). Real OSAlarms
      // fire through the decrementer handler below, as on hardware/Dolphin.

      // Periodic scheduler dump: diagnostics only (NWII_SAMPLE).
      static uint32_t dcount = 0;
      if (diag.sample && (dcount++ % 500) == 0) {
          uint32_t cur = ctx.mmu.read32(0x800000E4);
          uint32_t th = ctx.mmu.read32(0x800000DC);
          std::cout << "[Sched] cur=0x" << std::hex << cur << std::dec << "\n";
          int guard = 0;
          while (th != 0 && guard++ < 24) {
              uint16_t state = ctx.mmu.read16(th + 0x2C8);
              uint32_t prio = ctx.mmu.read32(th + 0x2D0);
              uint32_t wq = ctx.mmu.read32(th + 0x2DC);
              uint32_t srr0 = ctx.mmu.read32(th + 0x198);
              std::cout << "  th=0x" << std::hex << th << (th==cur?"*":" ")
                        << " state=" << std::dec << state << " prio=" << prio
                        << " waitq=0x" << std::hex << wq << " srr0=0x" << srr0
                        << std::dec << "\n";

              

              if (wq >= 0x80000000u && wq < 0x81800000u && diag.sample) {
                  std::cout << "    wq[-2..5]:" << std::hex;
                  for (int i = -2; i < 6; ++i)
                      std::cout << " " << ctx.mmu.read32(wq + i * 4);
                  std::cout << std::dec << "\n";
              }
              th = ctx.mmu.read32(th + 0x2FC);
          }
      }
      uint32_t dec_handler = ctx.mmu.read32(0x80003000 + 8 * 4);
      uint32_t current_ctx = ctx.mmu.read32(0x800000D4);
      if (dec_handler != 0 && dec_handler != 0xFFFFFFFF && current_ctx != 0) {
          nwii::runtime::prof::bump(nwii::runtime::prof::kDecIrq);
          hle_save_context_to_guest(ctx, current_ctx);
          ctx.dispatch_saved_ctx = current_ctx;
          ctx.srr0 = ctx.pc;
          ctx.srr1 = ctx.msr;
          ctx.msr &= ~0x8000;
          ctx.gpr[3] = 8;           
          ctx.gpr[4] = current_ctx; 

          
          
          ctx.lr = 0xFFFFFFFC;
          ctx.pc = dec_handler;
          longjmp(ctx.exception_jmp_buf, 1);
      }
  }

  uint32_t active_ints = nwii::runtime::hw::pi_intsr & nwii::runtime::hw::pi_intmr;

  
  if (active_ints != 0 && (ctx.msr & 0x8000)) {

      

      
      
      bool sched_disabled = false;
      uint32_t r13 = ctx.gpr[13];
      uint32_t lock_off = nwii::runtime::Config::get().game_profile.sched_lock_offset;
      if (lock_off != 0 && r13 >= 0x80200000u && r13 < 0x81800000u) {
          uint32_t lock_addr = r13 - lock_off;
          if (lock_addr >= 0x80000000u && lock_addr < 0x82000000u) {
              uint32_t lock_val = ctx.mmu.read32(lock_addr);

              sched_disabled = (lock_val > 0 && lock_val < 256);
          }
      }
      bool handler_in_flight = (ctx.dispatch_saved_ctx != 0);

      

      
      bool use_new_path =
          (nwii::runtime::Config::get().game_profile.ext_interrupt_dispatch != 0);

      

      
      bool idle = (ctx.mmu.read32(0x800000E4) == 0);

      

      

      if (use_new_path && (handler_in_flight || (!idle && sched_disabled))) {
          
          if (diag.sample) {
              static uint64_t defer_count = 0;
              if ((++defer_count % 500000) == 0)
                  std::cout << "[HLE PI] int deferred: sched_disabled="
                            << sched_disabled << " in_flight=" << handler_in_flight
                            << " pc=0x" << std::hex << ctx.pc << std::dec << "\n";
          }
      } else {

          
          static uint32_t s_dispatch_addr = 0xFFFFFFFF; 
          if (s_dispatch_addr == 0xFFFFFFFF) {
              
              uint32_t from_cfg = nwii::runtime::Config::get().game_profile.ext_interrupt_dispatch;
              if (from_cfg != 0) {
                  s_dispatch_addr = from_cfg;
                  std::cout << "[HLE PI] __OSDispatchInterrupt from config: 0x"
                            << std::hex << s_dispatch_addr << std::dec << "\n";
              } else {

                  
                  s_dispatch_addr = 0;
              }
          }

          if (s_dispatch_addr != 0) {

              

              

              

              

              ctx.srr0 = ctx.pc;
              ctx.srr1 = ctx.msr;

              
              
              uint32_t current_ctx = ctx.mmu.read32(0x800000D4);
              if (current_ctx == 0) {
                  current_ctx = ctx.mmu.read32(0x800000D0);
              }
              if (current_ctx == 0) {
                  std::cout << "[HLE PI] FATAL: current_ctx is 0! pc=0x" << std::hex << ctx.pc << " intmr=0x" << nwii::runtime::hw::pi_intmr << " intsr=0x" << nwii::runtime::hw::pi_intsr << std::dec << "\n";
              }

              
              
              if (current_ctx != 0) {
                  hle_save_context_to_guest(ctx, current_ctx);
              }

              
              ctx.gpr[3] = 0x04;
              
              ctx.gpr[4] = current_ctx;
              if (nwii::runtime::prof::g_on) {
                  nwii::runtime::prof::bump(nwii::runtime::prof::kExtIrq);
                  nwii::runtime::prof::count_irq_causes(active_ints);
              }

              
              ctx.msr &= ~0x8000u; 

              
              
              ctx.lr = 0xFFFFFFFC;
              ctx.pc = s_dispatch_addr;

              if (diag.sample) {
                  static uint32_t dc = 0;
                  if ((dc++ % 1000) == 0)
                      std::cout << "[HLE PI] -> __OSDispatchInterrupt #" << std::dec
                                << dc << " pc=0x" << std::hex << ctx.pc
                                << " cur=0x" << ctx.mmu.read32(0x800000E4)
                                << std::dec << "\n";
              }

              longjmp(ctx.exception_jmp_buf, 1);

          } else {

              
              int os_intr = -1;
              uint32_t bit_to_clear = 0;
              
              if      (active_ints & 0x00004000) { os_intr = 27; bit_to_clear = 0x00004000; } 
              else if (active_ints & 0x00000100) { os_intr = 24; bit_to_clear = 0x00000100; } 
              else if (active_ints & 0x00000004) { os_intr = 21; bit_to_clear = 0x00000004; } 
              else if (active_ints & 0x00000008) { os_intr = 20; bit_to_clear = 0x00000008; } 
              else if (active_ints & 0x00000010) { os_intr =  9; bit_to_clear = 0x00000010; } 

              else if (active_ints & 0x00000040) { os_intr = nwii::runtime::hw::dsp_pending_os_interrupt(); bit_to_clear = 0x00000040; } 
              else if (active_ints & 0x00000020) { os_intr =  8; bit_to_clear = 0x00000020; } 
              else if (active_ints & 0x00000400) { os_intr = 19; bit_to_clear = 0x00000400; } 
              else if (active_ints & 0x00000200) { os_intr = 18; bit_to_clear = 0x00000200; } 
              else if (active_ints & 0x00000800) { os_intr = 17; bit_to_clear = 0x00000800; } 

              if (os_intr != -1) {
                  uint32_t handler = ctx.mmu.read32(0x80003040 + os_intr * 4);

                  if (handler != 0 && handler != 0xFFFFFFFF) {
                      if (diag.sample && os_intr == 24) {
                          static int vn = 0;
                          if (vn++ < 5)
                              std::cout << "[VIdisp] #" << vn << " handler=0x"
                                        << std::hex << handler << " pc=0x"
                                        << ctx.pc << std::dec << "\n";
                      }
                      
                      static uint32_t vi_dispatch_count = 0;
                      if (diag.sample && os_intr != 24 && (vi_dispatch_count++ % 256) == 0) {
                          std::cout << "[HLE PI] Dispatching interrupt "
                                    << std::dec << os_intr << " to handler 0x"
                                    << std::hex << handler << " r1=0x"
                                    << ctx.gpr[1] << " pc=0x" << ctx.pc
                                    << std::dec << std::endl;
                      }
                      if (diag.sample) {
                          static uint64_t census[32] = {0};
                          static uint64_t total = 0;
                          if (os_intr >= 0 && os_intr < 32) census[os_intr]++;
                          if ((++total % 2000) == 0) {
                              std::cout << "[IntCensus]";
                              for (int i = 0; i < 32; ++i)
                                  if (census[i])
                                      std::cout << " " << std::dec << i
                                                << ":" << census[i];
                              std::cout << "\n";
                          }
                      }
                      uint32_t current_ctx = ctx.mmu.read32(0x800000D4);
                      if (current_ctx == 0) {
                          
                          if (diag.sample) {
                              static int n = 0;
                              if (n++ < 4)
                                  std::cout << "[HLE PI] int " << std::dec
                                            << os_intr << " deferred: no current context\n";
                          }
                      } else {
                          
                          nwii::runtime::hw::pi_intsr &= ~bit_to_clear;
                          hle_save_context_to_guest(ctx, current_ctx);
                          ctx.dispatch_saved_ctx = current_ctx;
                          ctx.srr0 = ctx.pc;
                          ctx.srr1 = ctx.msr;
                          ctx.msr &= ~0x8000u; 
                          ctx.gpr[3] = os_intr;
                          ctx.gpr[4] = current_ctx;
                          ctx.lr = 0xFFFFFFFC;
                          ctx.pc = handler;
                          ctx.ext_resched_pending = true;
                          longjmp(ctx.exception_jmp_buf, 1);
                      }

                      ctx.msr &= ~0x8000u;
                      CPUContext::BackupState bk;
                      bk.gpr = ctx.gpr;
                      bk.fpr = ctx.fpr;
                      bk.ps1 = ctx.ps1;
                      bk.cr  = ctx.cr;
                      bk.lr  = ctx.lr;
                      bk.ctr = ctx.ctr;
                      bk.xer = ctx.xer;
                      bk.pc  = ctx.pc;
                      bk.srr0 = ctx.srr0;
                      bk.srr1 = ctx.srr1;
                      bk.msr = ctx.msr | 0x8000;
                      bk.fpscr = ctx.fpscr;
                      bk.gqr = ctx.gqr;
                      bk.sprg = ctx.sprg;
                      ctx.backup_stack.push(bk);

                      ctx.in_callback = true;
                      ctx.callback_depth++;
                      ctx.srr0 = ctx.pc;
                      ctx.srr1 = ctx.msr | 0x8000;
                      ctx.gpr[3] = os_intr;
                      ctx.gpr[4] = 0;
                      ctx.lr = 0xFFFFFFFC;
                      ctx.pc = handler;
                      longjmp(ctx.exception_jmp_buf, 1);
                  } else {
                      
                      static uint32_t last_null_pc = 0;
                      static int null_warn_count = 0;
                      if (ctx.pc != last_null_pc) {
                          null_warn_count = 0;
                          last_null_pc = ctx.pc;
                      }
                      if (null_warn_count++ < 3)
                          std::cout << "[HLE PI] Handler for interrupt "
                                    << std::dec << os_intr
                                    << " is NULL (OS reinit in progress, deferring)."
                                    << std::endl;
                  }
              }
          } 
      } 
  } else if (nwii::runtime::hw::pi_intsr != 0) {
      
  }

  if ((ctx.msr & 0x8000) == 0)
    return false;

  if (ctx.pending_cb_count.load(std::memory_order_relaxed) == 0)
    return false;

  CallbackInfo cb;
  {
    std::lock_guard<std::mutex> lock(ctx.cb_mutex);
    if (ctx.pending_callbacks.empty())
      return false;
    cb = ctx.pending_callbacks.front();
    ctx.pending_callbacks.pop();
    ctx.pending_cb_count.fetch_sub(1, std::memory_order_relaxed);
  }

  std::cout << "[HLE Callback] Dispatching cb=0x" << std::hex << cb.cb_addr
            << " arg1=0x" << cb.arg1 << " arg2=0x" << cb.arg2 << " from PC=0x"
            << ctx.pc << " LR=0x" << ctx.lr << " r1=0x" << ctx.gpr[1]
            << std::dec << std::endl;

  uint32_t current_ctx = ctx.mmu.read32(0x800000D4);
  if (current_ctx != 0) {

    hle_save_context_to_guest(ctx, current_ctx);
    ctx.dispatch_saved_ctx = current_ctx;

    ctx.srr0 = ctx.pc;
    ctx.srr1 = ctx.msr;
    ctx.msr &= ~0x8000; 

    ctx.gpr[3] = cb.arg1;
    ctx.gpr[4] = cb.arg2;

    ctx.lr = 0xFFFFFFFC;
    ctx.pc = cb.cb_addr;
    longjmp(ctx.exception_jmp_buf, 1);
  }

  CPUContext::BackupState bk;
  bk.gpr = ctx.gpr;
  bk.fpr = ctx.fpr;
  bk.ps1 = ctx.ps1;
  bk.cr  = ctx.cr;
  bk.lr  = ctx.lr;
  bk.ctr = ctx.ctr;
  bk.xer = ctx.xer;
  bk.pc  = ctx.pc;
  bk.srr0 = ctx.srr0;
  bk.srr1 = ctx.srr1;
  bk.msr = ctx.msr;
  bk.fpscr = ctx.fpscr;
  bk.gqr = ctx.gqr;
  bk.sprg = ctx.sprg;
  ctx.backup_stack.push(bk);

  ctx.in_callback = true;
  ctx.callback_depth++;

  ctx.gpr[3] = cb.arg1;
  ctx.gpr[4] = cb.arg2;

  uint32_t cb_stack_top = get_callback_stack_top() - (ctx.callback_depth - 1) * 4096;
  ctx.gpr[1] = cb_stack_top - 16;
  ctx.mmu.write32(ctx.gpr[1], 0);
  ctx.mmu.write32(ctx.gpr[1] + 4, 0xFFFFFFFC);

  ctx.lr = 0xFFFFFFFC;
  ctx.pc = cb.cb_addr;
  longjmp(ctx.exception_jmp_buf, 1);
}

// ---------------------------------------------------------------------------
// PERF2: dispatcher shortcut for interpreted targets.
//
// The generated dispatcher (run_game) does, per iteration:
//   [loop top: pc_history; pc == 0 -> idle path]  setjmp;
//   process_pending_callbacks(ctx);  pc == 0xFFFFFFFC -> callback return;
//   normalise pc (0xC.../0xD... -> 0x8.../0x9..., < 0x80000000 -> |0x80000000);
//   ++inst_count;  switch (pc) { DOL cases ...
//     default: linear scan of all ~29.5k g_func_bounds, else interpret_step(ctx); }
// For a target outside the recompiled DOL text the scan can never match (every
// bound and every case label lies inside the text sections the loader
// registers with add_recompiled_range; checked by
// outputs/pc-tests/perf2/check_dispatch_invariant.py), so it only burns time:
// ~27 us per REL entry, 47% of all CPU-thread samples in the F000 load
// (PERF2 profile 20261001-172018).
//
// When the poll is called from run_game with such a target, we do the
// dispatcher's remaining steps ourselves -- normalise, ++inst_count,
// interpret_step -- and then run the poll the next dispatcher iteration would
// run, repeating while the next target is again interpreted. A pc of 0 or a
// stopped CPU after interpret_step restarts the dispatcher loop through its
// setjmp (longjmp, as an interrupt does), so its loop-top handling runs
// exactly as before. Same guest instruction stream, same inst_count, same
// poll points; only the host-side scan is skipped (and the NWII_SAMPLE-only
// "Dispatcher PC" print and pc_history ring are not updated for these hops).
// The caller is identified by its return address inside run_game's extent,
// registered by main.cpp (set_dispatcher_function). NWII_DISPATCH_SHORTCUT=0
// turns it off (negative control / A-B).
// ---------------------------------------------------------------------------
namespace {
uintptr_t g_dispatch_lo = 0;
uintptr_t g_dispatch_len = 0; // 0 = shortcut off: every return address fails the check
const bool g_dispatch_shortcut_off = [] {
  const char *e = std::getenv("NWII_DISPATCH_SHORTCUT");
  return e && e[0] == '0';
}();

bool dispatch_interpreted_targets(CPUContext &ctx, bool r) {
  for (;;) {
    uint32_t pc = ctx.pc;
    if (pc == 0 || pc == 0xFFFFFFFCu)
      return r; // the dispatcher handles these itself, as before
    if ((pc & 0xF0000000u) == 0xC0000000u) pc = (pc & 0x0FFFFFFFu) | 0x80000000u;
    else if ((pc & 0xF0000000u) == 0xD0000000u) pc = (pc & 0x0FFFFFFFu) | 0x90000000u;
    else if (pc < 0x80000000u) pc |= 0x80000000u;
    if (in_recompiled_code(pc))
      return r; // a DOL target: the dispatcher's switch takes it
    ctx.pc = pc;
    ++ctx.inst_count; // the dispatcher's per-dispatch count
    nwii::runtime::prof::bump(nwii::runtime::prof::kDispatchShortcut);
    interpret_step(ctx);
    if (ctx.pc == 0 || !ctx.is_running)
      longjmp(ctx.exception_jmp_buf, 1); // dispatcher loop top: pc==0 / exit
    r = poll_body(ctx); // the next dispatcher iteration's poll
  }
}
} // namespace

// ---------------------------------------------------------------------------
// PERF2: guest idle loops.
//
// Dolphin's idle-skip pattern (PPCAnalyst / Jit "bSkipIdle"):
//   lwz r0, d(r13)   (0x800Dxxxx)
//   cmplwi r0, 0     (0x28000000)  or, on Wii, cmpwi r0, 0 (0x2C000000)
//   beq -8           (0x4182FFF8)
// spins until an interrupt handler changes the r13-relative word, i.e. the
// guest has nothing to do (SOUE01: SelectThread's run-queue wait 0x803ACB68 and
// 13 other OS/driver waits). The recompiled code polls at the beq with
// ctx.pc = its address. idle_loop_scan() finds these back-edges in the DOL text
// once at start-up; the poll then recognises an idle iteration with one range
// compare. On an idle iteration (nothing delivered, cr0.eq so the loop spins
// on) the poll lets the GPU catch up (GX_SyncOnIdle, see gx.cpp), as Dolphin
// flushes the GPU when the CPU idles.
// ---------------------------------------------------------------------------
int recompiled_range_count();
void recompiled_range_get(int i, uint32_t &start, uint32_t &end);
bool GX_SyncOnIdle();

namespace {
constexpr int kMaxIdlePcs = 64;
uint32_t g_idle_pcs[kMaxIdlePcs];
int32_t g_idle_disp[kMaxIdlePcs]; // d of the loop's lwz r0, d(r13)
int g_idle_n = 0;
uint32_t g_idle_lo = 0xFFFFFFFFu; // [g_idle_lo, g_idle_lo + g_idle_span]
uint32_t g_idle_span = 0;
const bool g_idle_off = [] {
  const char *e = std::getenv("NWII_IDLE");
  return e && e[0] == '0';
}();

// True when `pc` is a known idle back-edge AND the loop will spin on: the word
// it polls is 0 right now. cr0 is not used: after an interrupt returns to the
// beq, cr0/r0 are the values from before the handler ran (which may just have
// set the word), so the loop goes round once more and leaves. Treating that
// pass as idle skipped a whole VI period per wake-up (found by NWII_IDLE_TRACE).
inline bool idle_iteration(CPUContext &ctx, uint32_t pc) {
  for (int i = 0; i < g_idle_n; ++i)
    if (g_idle_pcs[i] == pc)
      return ctx.mmu.read32(ctx.gpr[13] + (uint32_t)g_idle_disp[i]) == 0;
  return false;
}

// PERF2 timing model (roadmap M2.6), on by default; NWII_TIMING=legacy turns
// all three parts off (TB = inst_count, no skip, no throttle). It consists of:
//  - the Dolphin TB ratio (cpu_context.h g_tb_div = 12),
//  - idle skipping: an idle iteration fast-forwards the clock to the earliest
//    pending event (scheduler: VI, DI/AR/DSP completions; the decrementer),
//    which is what spinning in the loop would reach, only without executing
//    the spins (Dolphin CoreTiming::Idle under "SkipIdle");
//  - a real-time throttle: the idle guest may not run its clock ahead of the
//    wall clock, so idle-bound scenes advance at real-time speed (60 VI/s);
//    a guest that is behind (CPU-bound) is never slowed, the anchor just moves
//    (slow motion instead of catch-up, as Dolphin's throttle).
// NWII_IDLE_SKIP=0/1 and NWII_THROTTLE=0/1 override the two parts.
struct TimingOpts {
  bool idle_skip = false;
  bool throttle = false;
};
TimingOpts load_timing_opts() {
  TimingOpts t;
  t.idle_skip = t.throttle = true;
  if (const char *e = std::getenv("NWII_TIMING"))
    if (std::strcmp(e, "legacy") == 0) t.idle_skip = t.throttle = false;
  if (const char *e = std::getenv("NWII_IDLE_SKIP")) t.idle_skip = e[0] == '1';
  if (const char *e = std::getenv("NWII_THROTTLE")) t.throttle = e[0] == '1';
  if (!t.idle_skip) t.throttle = false; // the throttle lives in the skip path
  return t;
}
const TimingOpts g_timing = load_timing_opts();
// NWII_IDLE_TRACE=<n>: print the first n idle skips (diagnostic, off by default).
const long g_idle_trace = [] {
  const char *e = std::getenv("NWII_IDLE_TRACE");
  return e ? std::atol(e) : 0L;
}();
long g_idle_traced = 0;

struct ThrottleState {
  bool anchored = false;
  std::chrono::steady_clock::time_point wall0;
  uint64_t tb0 = 0;
};
ThrottleState g_throttle;

// Anything that should end an idle wait early and be seen by the next poll.
inline bool async_wakeup_pending(CPUContext &ctx) {
  return (nwii::runtime::hw::pi_intsr & nwii::runtime::hw::pi_intmr) != 0 ||
         ctx.vblank_pending.load(std::memory_order_relaxed) ||
         ctx.pending_cb_count.load(std::memory_order_relaxed) != 0 ||
         !ctx.is_running.load(std::memory_order_relaxed);
}

// Returns the timebase value the clock may be advanced to, <= target_tb.
uint64_t throttle_to(CPUContext &ctx, uint64_t now_tb, uint64_t target_tb) {
  using clk = std::chrono::steady_clock;
  ThrottleState &t = g_throttle;
  const clk::time_point now = clk::now();
  if (!t.anchored) {
    t.anchored = true;
    t.wall0 = now;
    t.tb0 = now_tb;
  }
  const double tb_per_s = (double)ctx.tb_freq;
  auto wall_of = [&](uint64_t tb) {
    return t.wall0 + std::chrono::duration_cast<clk::duration>(
                         std::chrono::duration<double>((double)(int64_t)(tb - t.tb0) / tb_per_s));
  };
  // Behind real time by more than 50 ms (CPU-bound stretch): re-anchor so the
  // guest is not run faster afterwards to catch up. Ahead by more than 250 ms
  // can only come from a clock discontinuity (e.g. a cache-range HLE that
  // accounts 2^32 loop iterations, seen as a ~1.4e13-instruction jump during
  // CheckSave): re-anchor too instead of sleeping it off.
  const clk::time_point virt_now = wall_of(now_tb);
  if (virt_now + std::chrono::milliseconds(50) < now ||
      virt_now > now + std::chrono::milliseconds(250)) {
    t.wall0 = now;
    t.tb0 = now_tb;
  }
  const clk::time_point due = wall_of(target_tb);
  for (;;) {
    const clk::time_point w = clk::now();
    if (w >= due) return target_tb;
    if (async_wakeup_pending(ctx)) {
      // Advance only as far as real time has gone.
      const double el = std::chrono::duration<double>(w - t.wall0).count();
      const uint64_t real_tb = t.tb0 + (uint64_t)(el * tb_per_s);
      return std::max(now_tb, std::min(target_tb, real_tb));
    }
    const auto left = due - w;
    std::this_thread::sleep_for(left < std::chrono::milliseconds(2) ? left
                                                                    : clk::duration(std::chrono::milliseconds(2)));
  }
}

void on_guest_idle(CPUContext &ctx) {
  if (GX_SyncOnIdle()) {
    // New commands may have raised PE DrawDone/token: let the next poll see them.
    nwii::runtime::prof::bump(nwii::runtime::prof::kGxIdleParse);
    return;
  }
  if (!g_timing.idle_skip) return;
  // Poll-count delays (IPC/DI replies) and queued callbacks advance per poll:
  // keep spinning so they are not reordered against the clock.
  if (g_ipc_interrupt_delay > 0 || nwii::runtime::hw::g_di_interrupt_delay > 0 ||
      async_wakeup_pending(ctx))
    return;
  const uint64_t now_tb = ctx.read_timebase();
  uint64_t target = g_poll_sched.next_due();
  if (ctx.dec_irq_pending) target = std::min(target, ctx.dec_written_tb + ctx.dec_value);
  if (target == UINT64_MAX || target <= now_tb) return;
  // One VI period at most per skip, so host-side state (AI, input) is polled.
  target = std::min(target, now_tb + std::max<uint64_t>(1, ctx.tb_freq / 60));
  if (g_timing.throttle) target = throttle_to(ctx, now_tb, target);
  if (target <= now_tb) return;
  // CLOCK15: read_timebase() == target afterwards (was target * g_tb_div, which
  // ignored the time base's boot value ctx.tb_base).
  const uint64_t new_inst = ctx.inst_for_tb(target);
  if (new_inst <= ctx.inst_count) return;
  if (g_idle_traced < g_idle_trace) {
    ++g_idle_traced;
    std::cout << "[IDLESKIP] pc=0x" << std::hex << ctx.pc << " lr=0x" << ctx.lr << std::dec
              << " now_tb=" << now_tb << " next_due=" << g_poll_sched.next_due()
              << " dec=" << (ctx.dec_irq_pending ? (int64_t)(ctx.dec_written_tb + ctx.dec_value) : -1)
              << " -> " << target << " msr=0x" << std::hex << ctx.msr << " intsr=0x"
              << nwii::runtime::hw::pi_intsr.load() << " intmr=0x" << nwii::runtime::hw::pi_intmr.load()
              << " waitword=0x"
              << ctx.mmu.read32(ctx.gpr[13] + (int32_t)(int16_t)(ctx.mmu.read32(ctx.pc - 8) & 0xFFFF))
              << " r0=0x" << ctx.gpr[0] << " cr0eq=" << ctx.cr[0].eq << std::dec << '\n';
  }
  nwii::runtime::prof::bump(nwii::runtime::prof::kIdleSkips);
  nwii::runtime::prof::bump(nwii::runtime::prof::kIdleSkipTicks, new_inst - ctx.inst_count);
  ctx.inst_count = new_inst;
}
} // namespace

void idle_loop_scan(CPUContext &ctx) {
  g_idle_n = 0;
  if (g_idle_off) {
    std::cout << "[PERF2] idle-loop handling off (NWII_IDLE=0)" << std::endl;
    return;
  }
  uint32_t lo = 0xFFFFFFFFu, hi = 0;
  for (int r = 0; r < recompiled_range_count(); ++r) {
    uint32_t s = 0, e = 0;
    recompiled_range_get(r, s, e);
    for (uint32_t a = s; a + 12 <= e && g_idle_n < kMaxIdlePcs; a += 4) {
      const uint32_t w0 = ctx.mmu.read32(a);
      if ((w0 & 0xFFFF0000u) != 0x800D0000u) continue;
      const uint32_t w1 = ctx.mmu.read32(a + 4);
      if (w1 != 0x28000000u && w1 != 0x2C000000u) continue;
      if (ctx.mmu.read32(a + 8) != 0x4182FFF8u) continue;
      g_idle_disp[g_idle_n] = (int32_t)(int16_t)(w0 & 0xFFFF);
      g_idle_pcs[g_idle_n++] = a + 8;
      lo = std::min(lo, a + 8);
      hi = std::max(hi, a + 8);
    }
  }
  if (g_idle_n) {
    g_idle_lo = lo;
    g_idle_span = hi - lo;
  }
  std::cout << "[PERF2] timing: tb_div " << std::dec << g_tb_div << ", idle skip "
            << (g_timing.idle_skip ? "on" : "off") << ", throttle "
            << (g_timing.throttle ? "on" : "off") << std::endl;
  std::cout << "[PERF2] idle loops: " << std::dec << g_idle_n << std::hex;
  for (int i = 0; i < g_idle_n; ++i) std::cout << " 0x" << g_idle_pcs[i];
  std::cout << std::dec << std::endl;
}

void set_dispatcher_function(const void *fn) {
  uintptr_t lo = 0, len = 0;
  if (g_dispatch_shortcut_off || !fn ||
      !nwii::runtime::prof::host_function_extent(fn, lo, len)) {
    std::cout << "[PERF2] dispatcher shortcut off" << std::endl;
    return;
  }
  g_dispatch_lo = lo;
  g_dispatch_len = len;
  std::cout << "[PERF2] dispatcher shortcut on (run_game extent 0x" << std::hex << lo
            << "+0x" << len << std::dec << ")" << std::endl;
}

NWII_POLL_NOINLINE bool process_pending_callbacks(CPUContext &ctx) {
  const uint32_t entry_pc = ctx.pc;
  const bool r = poll_body(ctx);
  if (entry_pc - g_idle_lo <= g_idle_span) {
    // An idle-loop back-edge (DOL text, never a dispatcher REL target): the
    // loop spins on while cr0.eq, unless this poll redirected execution.
    if (!r && idle_iteration(ctx, entry_pc))
      on_guest_idle(ctx);
    return r;
  }
  if (entry_pc == 0 || NWII_RETURN_ADDRESS() - g_dispatch_lo >= g_dispatch_len)
    return r;
  return dispatch_interpreted_targets(ctx, r);
}

} 
