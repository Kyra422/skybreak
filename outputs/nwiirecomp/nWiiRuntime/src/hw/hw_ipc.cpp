#include "runtime/hw/hw.h"
#include "runtime/config.h"
#include "runtime/cpu_context.h"
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <deque>

namespace nwii::runtime {
    extern CPUContext* g_ctx_ptr;
    extern MMU* g_mmu;
    extern int g_ipc_interrupt_delay;
}

namespace nwii::runtime::hw {

// Read once at start-up (M0.3: no std::getenv per ARMCTRL write).
static const bool g_ipc_sample = std::getenv("NWII_SAMPLE") != nullptr;

static uint32_t ipc_arm_msg = 0;
static uint32_t ipc_arm_ctrl = 0;
static uint32_t ipc_ppc_ctrl = 0;
uint32_t ipc_ppc_msg = 0;
static uint32_t ipc_irq_flags = 0;
static uint32_t ipc_irq_mask = 0x40000000;
struct PendingReply { uint32_t address; int interrupt_delay; };
static std::deque<PendingReply> ipc_reply_queue;
static bool ipc_reply_in_flight = false;

static void publish_next_reply() {
  if (ipc_reply_in_flight || ipc_reply_queue.empty()) return;
  const auto reply = ipc_reply_queue.front();
  ipc_arm_msg = reply.address;
  ipc_reply_queue.pop_front();
  ipc_reply_in_flight = true;
  ipc_arm_ctrl |= 0x02;
  ipc_ppc_ctrl |= 0x04;
  g_ipc_interrupt_delay = reply.interrupt_delay;
}

static void enqueue_reply(uint32_t req_addr, int interrupt_delay = 50) {
  ipc_reply_queue.push_back({req_addr & 0x1FFFFFFF, interrupt_delay});
  publish_next_reply();
}

void ipc_update_interrupts() {
  // Y1/reply and Y2/ack are gated by their separate PPC enables.
  if (((ipc_ppc_ctrl & 0x14) == 0x14) ||
      ((ipc_ppc_ctrl & 0x22) == 0x22))
    ipc_irq_flags |= 0x40000000;
  if (ipc_irq_flags & ipc_irq_mask)
    trigger_pi_interrupt(0x4000);
  else
    clear_pi_interrupt(0x4000);
}

extern "C" int32_t handle_ios_ipc(nwii::runtime::CPUContext& ctx, uint32_t request_addr);

int32_t dispatch_ipc(CPUContext& ctx, uint32_t virt_addr) {
  int32_t result = handle_ios_ipc(ctx, virt_addr);

  // Several requests can complete while PPC is handling a previous reply.
  // Keep that mailbox stable until PPC acknowledges receipt with X2.
  ipc_arm_ctrl |= 0x01;
  if (result != -0x70000001) enqueue_reply(virt_addr);
  
  return result;
}

int32_t ipc_dispatch_request(CPUContext &ctx, uint32_t req_addr) {
  uint32_t virt_addr = req_addr;
  if ((virt_addr & 0xF0000000) == 0xC0000000) virt_addr = (virt_addr & 0x0FFFFFFF) | 0x80000000;
  if ((virt_addr & 0xF0000000) == 0xD0000000) virt_addr = (virt_addr & 0x0FFFFFFF) | 0x90000000;

  return dispatch_ipc(ctx, virt_addr);
}

void hle_set_ipc_arm_msg(uint32_t req_addr) {
  enqueue_reply(req_addr, 5000);
}



void ipc_post_reply(uint32_t req_addr) {
  // Async completions (DI reads, USB HCI events) need the same reply layout as the
  // synchronous path in ios.cpp handle_ios_ipc (PERF3 (c), Dolphin Kernel::EnqueueIPCReply):
  // answered command into +8, IPC_REPLY (8) into +0. Otherwise the SDK IpcReplyHandler
  // switches on the fd and runs the wrong cache/fix-up branch. NWII_IPC_REPLY_CMD=0: old layout.
  static const bool reply_cmd = [] {
    const char *e = std::getenv("NWII_IPC_REPLY_CMD");
    return !(e && e[0] == '0');
  }();
  if (reply_cmd && nwii::runtime::g_mmu) {
    const uint32_t virt = req_addr | 0x80000000u; // MEM1 0x0... -> 0x8..., MEM2 0x1... -> 0x9...
    const uint32_t cmd = nwii::runtime::g_mmu->read32(virt);
    if (cmd != 8) {
      nwii::runtime::g_mmu->write32(virt + 8, cmd);
      nwii::runtime::g_mmu->write32(virt, 8);
    }
  }
  // Completion of an already acknowledged request does not acknowledge, or
  // clear the send state of, a different request.
  enqueue_reply(req_addr);
}

void register_ipc(MMIODispatcher& dispatcher) {{
    dispatcher.register_region(0xCD000000, 0xCD0003FF,
        [](uint32_t addr) -> uint32_t {
            switch (addr & 0x00FFFFFF) {
            case 0x000000: return ipc_ppc_msg;
            case 0x000004: return ipc_ppc_ctrl;
            case 0x000008: return ipc_arm_msg;
            case 0x00000C: return ipc_arm_ctrl;
            case 0x000030: return ipc_irq_flags;
            case 0x000034: return ipc_irq_mask;
            default: return 0;
            }
        },
        [](uint32_t addr, uint32_t val) {
            switch (addr & 0x00FFFFFF) {
            case 0x000000: {
                ipc_ppc_msg = val;
                break;
            }
            case 0x000004: {
                ipc_ppc_ctrl = (ipc_ppc_ctrl & ~0x39) | (val & 0x39);
                // PPC writes acknowledge the two IOS-owned status bits.
                if (val & 0x04) ipc_ppc_ctrl &= ~0x04;
                if (val & 0x02) ipc_ppc_ctrl &= ~0x02;
                // SOUE01 clears Y1 before invoking the callback, then writes
                // X2 only after the callback returns. Clearing Y1 alone must
                // not expose another completion in the same mailbox.
                if ((val & 0x08) && !(ipc_ppc_ctrl & 0x04)) {
                    ipc_reply_in_flight = false;
                    ipc_ppc_ctrl &= ~0x08; // IOS consumes the acknowledgement.
                    publish_next_reply();
                }
                if (((val & 0x04) && (val & 0x10)) ||
                    ((val & 0x02) && (val & 0x20)))
                    ipc_irq_flags |= 0x40000000;

                if (val & 0x01) {
                    
                    ipc_ppc_ctrl |= 0x01;
                    int32_t result = 0;
                    if (g_ctx_ptr) {
                        
                        result = ipc_dispatch_request(*g_ctx_ptr, ipc_ppc_msg);
                    }
                    std::cout << "[IPC] Request sent from PPC! result=" << result << "\n";
                    if (result != -0x70000001) { 
                        
                        ipc_ppc_ctrl &= ~0x01;
                        ipc_ppc_ctrl |= 0x02; // Ack; the reply queue owns Y1.
                        
                        g_ipc_interrupt_delay = 50;
                    } else {

                        

                        ipc_ppc_ctrl &= ~0x01;
                        ipc_ppc_ctrl |= 0x02; // Ack only; async reply arrives later.
                        g_ipc_interrupt_delay = 50;
                    }
                }

                if (!(val & 0x01)) ipc_update_interrupts();
                break;
            }

            case 0x000030:
                ipc_irq_flags &= ~val;
                ipc_update_interrupts();
                break;
            case 0x000034:
                ipc_irq_mask = val;
                ipc_update_interrupts();
                break;

            case 0x00000C: {
                ipc_arm_ctrl &= ~(val & 0x03);
                if (!(ipc_arm_ctrl & 3)) {
                    if (g_ipc_sample && (pi_intsr & 0x4000)) {
                        static int n = 0;
                        if (n++ < 6)
                            std::cout << "[IPC] armctrl write 0x" << std::hex
                                      << val << " swallows pending irq\n" << std::dec;
                    }
                    clear_pi_interrupt(0x00004000);
                }
                break;
            }
            } 
        }
    );
    dispatcher.register_region(0x0D000000, 0x0D0003FF,
        [&dispatcher](uint32_t a) { return dispatcher.read32(a | 0xC0000000); },
        [&dispatcher](uint32_t a, uint32_t v) { dispatcher.write32(a | 0xC0000000, v); }
    );
}}

} 
