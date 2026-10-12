#include "runtime/hw/hw.h"
#include "runtime/cpu_context.h"
#include "runtime/gx/renderer.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <atomic>

namespace nwii::runtime {
extern CPUContext *g_ctx_ptr;
}

namespace nwii::runtime::hw {

// PE control register (0xCC00100A), Dolphin UPECtrlReg: bit 0 = token



static std::atomic<uint32_t> g_pe_ctrl = 0; 
static std::atomic<bool> g_pe_token_pending = false;
static std::atomic<bool> g_pe_finish_pending = false;

std::atomic<uint32_t> g_pe_sr = 0;



std::atomic<uint32_t> g_pe_token = 0;

// PEEK10: PE config registers 0xCC001000-0xCC001008 (16-bit; Dolphin
// PixelEngine: ZCONF, ALPHACONF, DSTALPHACONF, ALPHAMODE, ALPHAREAD). The
// SDK's GXPoke* setters write them; only ALPHAREAD (GXPokeAlphaRead: 0 =
// read 00, 1 = read FF, 2 = read the real alpha) changes behaviour, for EFB
// colour peeks. Bounding box (BP 0x55/0x56, PE 0x10-0x16) is not emulated:
// SS only clears it in GXInit and never reads it (no GXReadBoundingBox).
static std::atomic<uint16_t> g_pe_cfg[5] = {};

// ---------------------------------------------------------------------------
// PEEK10 (roadmap M3.6): CPU EFB access window, physical 0x08000000. Address
// decode as Dolphin MMU EFB_Read/EFB_Write: x = bits 2-11, y = bits 12-21,
// bit 22 = Z, bit 23 = Z+colour (unimplemented in Dolphin too). Values as
// Dolphin RenderBase::AccessEFB: Z = 24-bit (16-bit for RGB565_Z16), colour =
// 0xAARRGGBB quantised to the EFB pixel format, alpha per PE ALPHAREAD.
// The EFB contents come from the renderer's per-frame snapshot (gx::EfbPeek,
// renderer_gl.cpp). SS: GXPeekZ in d3d::fn_80017500 (sun flare occlusion,
// Link's focus point). NWII_EFB_PEEK=0: reads 0, writes dropped (as before).
// ---------------------------------------------------------------------------
static bool efb_access_off() {
    static const bool off = [] {
        const char *e = std::getenv("NWII_EFB_PEEK");
        return e && e[0] == '0';
    }();
    return off;
}
static int efb_log_budget() {
    static int n = [] {
        const char *e = std::getenv("NWII_EFB_PEEK_LOG");
        return e ? std::max(1, std::atoi(e)) : 0;
    }();
    return n;
}

static uint32_t efb_read(uint32_t paddr) {
    if (efb_access_off())
        return 0;
    const int x = (int)((paddr & 0xFFF) >> 2);
    const int y = (int)((paddr >> 12) & 0x3FF);
    uint32_t fmt = 0, v = 0;
    bool have = false;
    if (paddr & 0x00800000) {
        static bool once = false;
        if (!once) { once = true; std::printf("[PEEK10] unimplemented Z+colour EFB read @%08X\n", paddr); }
        return 0;
    }
    if (paddr & 0x00400000) {
        float d = 1.0f; // no snapshot yet: a cleared EFB (Z = far)
        have = gx::EfbPeek(true, x, y, &d, nullptr, &fmt);
        v = std::clamp<uint32_t>(static_cast<uint32_t>(d * 16777216.0f), 0u, 0xFFFFFFu);
        if (fmt == 2) // RGB565_Z16: Dolphin returns the 16-bit value
            v >>= 8;
    } else {
        uint8_t c[4] = {0, 0, 0, 0};
        have = gx::EfbPeek(false, x, y, nullptr, c, &fmt);
        v = ((uint32_t)c[3] << 24) | ((uint32_t)c[0] << 16) | ((uint32_t)c[1] << 8) | c[2];
        if (fmt == 1) { // RGBA6_Z24
            v &= 0xFCFCFCFCu;
            v |= (v >> 6) & 0x03030303u;
        } else if (fmt == 2) { // RGB565_Z16
            v &= 0xF8FCF8u;
            v |= (v >> 5) & 0x070007u;
            v |= (v >> 6) & 0x000300u;
        }
        if (fmt != 1)
            v |= 0xFF000000u;
        const uint16_t mode = g_pe_cfg[4].load(std::memory_order_relaxed) & 3;
        if (mode == 1)
            v |= 0xFF000000u;          // GX_READ_FF
        else if (mode != 2)
            v &= 0x00FFFFFFu;          // GX_READ_00
    }
    static std::atomic<int> logged{0};
    if (efb_log_budget() && logged.fetch_add(1) < efb_log_budget())
        std::printf("[PEEK10] peek%s (%d,%d) = %06X%s fmt=%u\n",
                    (paddr & 0x00400000) ? "Z" : "ARGB", x, y, v,
                    have ? "" : " (no snapshot yet)", fmt);
    return v;
}

static void efb_write(uint32_t paddr, uint32_t value) {
    if (efb_access_off())
        return;
    const int x = (int)((paddr & 0xFFF) >> 2);
    const int y = (int)((paddr >> 12) & 0x3FF);
    if (paddr & 0x00800000) {
        static bool once = false;
        if (!once) { once = true; std::printf("[PEEK10] unimplemented Z+colour EFB write @%08X\n", paddr); }
        return;
    }
    gx::EfbPoke((paddr & 0x00400000) != 0, x, y, value);
    static std::atomic<int> logged{0};
    if (efb_log_budget() && logged.fetch_add(1) < efb_log_budget())
        std::printf("[PEEK10] poke%s (%d,%d) = %08X\n",
                    (paddr & 0x00400000) ? "Z" : "ARGB", x, y, value);
}

// PI lines follow (pending && enabled), like Dolphin's UpdateInterrupts.
static void pe_update_interrupts() {
    g_pe_sr = (g_pe_token_pending ? 0x04u : 0u) | (g_pe_finish_pending ? 0x08u : 0u);
    if (g_pe_token_pending && (g_pe_ctrl & 0x01))
        trigger_pi_interrupt(0x200); 
    else
        clear_pi_interrupt(0x200);
    if (g_pe_finish_pending && (g_pe_ctrl & 0x02))
        trigger_pi_interrupt(0x400); 
    else
        clear_pi_interrupt(0x400);
}

void register_pe(MMIODispatcher &dispatcher) {
    nwii::runtime::g_efb_read_hook = &efb_read;   // PEEK10
    nwii::runtime::g_efb_write_hook = &efb_write;
    dispatcher.register_region(0xCC001000, 0xCC0010FF,
        [](uint32_t addr) -> uint32_t {
            if (addr <= 0xCC001008 && !(addr & 1))
                return g_pe_cfg[(addr - 0xCC001000) / 2].load();
            if (addr == 0xCC00100A)
                return g_pe_ctrl.load();
            
            if (addr == 0xCC00100E)
                return g_pe_token & 0xFFFF;
            return 0;
        },
        [](uint32_t addr, uint32_t val) {
            if (addr <= 0xCC001008 && !(addr & 1)) {
                g_pe_cfg[(addr - 0xCC001000) / 2] = (uint16_t)val;
                return;
            }
            if (addr == 0xCC00100A) {
                if (std::getenv("NWII_SAMPLE")) {
                    static int n = 0;
                    if (n++ < 12)
                        std::cout << "[PE] ctrl write @" << std::hex << addr
                                  << " val=0x" << val << std::dec << "\n";
                }
                if (val & 0x04) g_pe_token_pending = false;
                if (val & 0x08) g_pe_finish_pending = false;
                g_pe_ctrl = val & 0x03;
                pe_update_interrupts();
            }
        }
    );
}



void pe_signal_token(uint32_t token, bool raise_interrupt) {
    static int n = 0;
    if (n++ < 6)
        std::cout << "[PE] token 0x" << std::hex << token << (raise_interrupt ? " +int" : "") << std::dec << "\n";
    g_pe_token = token;
    if (raise_interrupt) {
        g_pe_token_pending = true;
        pe_update_interrupts();
    }
}

void pe_signal_finish() {
    static int n = 0;
    if (n++ < 6)
        std::cout << "[PE] finish\n";
    g_pe_finish_pending = true;
    pe_update_interrupts();
}

} 
