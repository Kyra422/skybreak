#include <iostream>
#include <vector>
#include "runtime/gx/fifo_parser.h"
#include "runtime/gx/gx_capture.h"
#include "runtime/gx_state.h"
#include "runtime/cpu_context.h"
#include "runtime/hw/hw.h"
#include "runtime/config.h"
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <chrono>
#include <map>
#include <unordered_map>
#include <algorithm>

namespace nwii::runtime {
    extern MMU* g_mmu;
}

namespace nwii::runtime::gx {

extern GXState g_state;

// NWII_SKINDBG state (declared early so ParseCP/ParseBP in the anonymous
// namespace can reference it).
// Parse-time shadow of the XF matrix memory: matrix loads (direct 0x10 and
// indexed 0x20-0x38) are applied here as the stream is parsed, so each draw
// can snapshot the matrix that is current at its point in the stream — before
// the renderer applies later loads over the same slot.
static float g_parse_posmtx[256] = {0};
static float g_parse_nrmmtx[96] = {0};

static void ParseApplyMatrixWrite(uint32_t xf_addr, const std::vector<float>& payload) {
    for (size_t i = 0; i < payload.size(); ++i) {
        uint32_t a = xf_addr + (uint32_t)i;
        if (a < 256) g_parse_posmtx[a] = payload[i];
        else if (a >= 0x400 && a < 0x400 + 96) g_parse_nrmmtx[a - 0x400] = payload[i];
    }
}

// RENDER1 (roadmap M1.2): register files in two places, each written strictly
// in stream order.
//  * g_parse_bp / g_parse_xf: parse-only shadows. The parser runs ahead of the
//    renderer (it parses the whole drained chunk, the renderer only the frame
//    cut out of it), so it must never write the renderer's register file --
//    that leaked 'future' BP values into render state (bp[0x41] blend/update,
//    konst selectors) and into the shader cache key. The parser reads these
//    for its own needs only: the 0x78 seam RMW merge base, need_normal (XF
//    0x100E/F) and the DL-cache state hash.
//  * g_state.bp: the render-time file, written by ApplyBPRegister() as the
//    renderer walks the commands. s_render_bpmask is its BP-mask register.
// Both apply BP 0xFE (BPMEM_BP_MASK) exactly like Dolphin's LoadBPReg:
//   new = (old & ~mask) | (val & mask); the mask resets to 0xFFFFFF after any
//   write to a register other than 0xFE.
// g3d's ResMatPix display lists write PE_CMODE0/1 and the konst selectors
// through the mask; without it they zeroed the colour/alpha update bits.
static uint32_t g_parse_bp[256] = {0};
static uint32_t g_parse_bpmask = 0xFFFFFF;
static uint32_t g_parse_xf[256] = {0};   // XF 0x1000-0x10FF
static uint32_t s_render_bpmask = 0xFFFFFF;

static inline uint32_t MaskedBP(uint32_t old_val, uint32_t val, uint32_t mask) {
    return ((old_val & ~mask) | (val & mask)) & 0xFFFFFF;
}

// PEEK10: NWII_XF_MATIDX=0 restores the pre-PEEK10 behaviour (XF 0x1018/0x1019
// writes do not change the default matrix indices; A/B switch only).
static bool XfMatIdxShared() {
    static const bool on = [] {
        const char* e = std::getenv("NWII_XF_MATIDX");
        return !(e && e[0] == '0');
    }();
    return on;
}

// XF register writes (0x1000-0x10FF) and matrix loads, parse side.
static void ParseApplyXF(uint32_t xf_addr, const std::vector<float>& payload) {
    if (xf_addr < 256 || (xf_addr >= 0x400 && xf_addr < 0x460))
        ParseApplyMatrixWrite(xf_addr, payload);
    for (size_t i = 0; i < payload.size(); ++i) {
        const uint32_t a = xf_addr + (uint32_t)i;
        if (a >= 0x1000 && a < 0x1100)
            std::memcpy(&g_parse_xf[a - 0x1000], &payload[i], 4);
        // PEEK10: XF 0x1018/0x1019 (MatrixIndexA/B) and CP 0x30/0x40 are one
        // register pair for the default (not per-vertex) position and texgen
        // matrices: Dolphin routes both writes through VertexShaderManager::
        // SetTexMatrixChangedA/B into xfmem.MatrixIndexA/B, last write wins.
        // The SDK writes both with one value, but g3d display lists may write
        // only XF 0x1018 -- SS's water draws select PNMTX0 for their screen-
        // projected texgen that way (before: identity = wrong refraction).
        if ((a == 0x1018 || a == 0x1019) && XfMatIdxShared()) {
            const uint32_t v = g_parse_xf[a - 0x1000];
            if (a == 0x1018) {
                g_state.cp[0x30] = v;
                g_state.defPosMtxIdx = v & 0x3F;
                for (int t = 0; t < 4; ++t)
                    g_state.defTexMtxIdx[t] = (v >> (6 + t * 6)) & 0x3F;
            } else {
                g_state.cp[0x40] = v;
                for (int t = 0; t < 4; ++t)
                    g_state.defTexMtxIdx[4 + t] = (v >> (t * 6)) & 0x3F;
            }
        }
    }
}

static bool g_skindbg = false;
static uint64_t g_skin_loaded_rows = 0;
static uint64_t g_skin_used_rows = 0;
static uint64_t g_skin_used_missing = 0;
static uint64_t g_skin_draws_total = 0, g_skin_draws_pmidx = 0, g_skin_defnz = 0, g_skin_snapped = 0;

namespace {
    inline bool gx_trace() {
        static bool t = std::getenv("NWII_GXTRACE") != nullptr;
        return t;
    }

    inline uint32_t Read24(const std::vector<uint8_t>& fifo, size_t offset) {
        return (fifo[offset] << 16) | (fifo[offset+1] << 8) | fifo[offset+2];
    }

    inline uint32_t Read32(const std::vector<uint8_t>& fifo, size_t offset) {
        return (fifo[offset] << 24) | (fifo[offset+1] << 16) | (fifo[offset+2] << 8) | fifo[offset+3];
    }

    void ApplyBPRegisterImpl(uint8_t reg, uint32_t val);

    // Parse-time BP write: updates the parse-only shadow (with the BP mask)
    // and raises the PE interrupts at the command's stream position. It does
    // NOT touch g_state.bp (M1.2: that file is render-time only).
    void ParseBP(uint8_t reg, uint32_t val) {
        const uint32_t nv = MaskedBP(g_parse_bp[reg], val, g_parse_bpmask);
        g_parse_bp[reg] = nv;
        if (reg == 0xFE) {
            g_parse_bpmask = nv;
            return;
        }
        g_parse_bpmask = 0xFFFFFF;

        if (reg == 0x45 && (nv & 0xF) == 2) {
            nwii::runtime::hw::pe_signal_finish();
        } else if (reg == 0x47) {
            nwii::runtime::hw::pe_signal_token(nv & 0xFFFF, false);
        } else if (reg == 0x48) {
            nwii::runtime::hw::pe_signal_token(nv & 0xFFFF, true);
        }
    }

    void ApplyBPRegisterImpl(uint8_t reg, uint32_t raw_val) {
        // Render-time BP write with the BP mask (see g_parse_bp above).
        const uint32_t val = MaskedBP(g_state.bp[reg], raw_val, s_render_bpmask);
        g_state.bp[reg] = val;
        if (reg == 0xFE) {
            s_render_bpmask = val;
            return;
        }
        s_render_bpmask = 0xFFFFFF;
        if (reg >= 0xE0 && reg <= 0xE7) {
            auto& bank = (val & 0x800000) ? g_state.tevKonstReg : g_state.tevColorReg;
            bank[reg - 0xE0] = val;
        } else if (reg == 0x00) {
            g_state.numTexGens = (val & 0xF);
            g_state.numChans = ((val >> 4) & 0x7);
            g_state.numTevStages = ((val >> 10) & 0xF) + 1;
            g_state.cullMode = (val >> 14) & 3;
        } else if (reg >= 0x28 && reg <= 0x2F) {

            
            int stage = (reg - 0x28) * 2;
            for (int half = 0; half < 2; ++half) {
                uint32_t f = val >> (half * 12);
                auto& s = g_state.tevStages[stage + half];
                bool enabled = (f >> 6) & 1;
                s.texMap = enabled ? (uint8_t)(f & 0x7) : 0xFF;
                s.texCoord = (f >> 3) & 0x7;
                s.colorChan = (f >> 7) & 0x7;
            }
        } else if (reg >= 0xC0 && reg <= 0xDF) {
            
            // Layout per Dolphin BPMemory TevStageCombiner — the inputs are

            
            int stage = (reg - 0xC0) / 2;
            auto& s = g_state.tevStages[stage];
            if (((reg - 0xC0) & 1) == 0) { 
                s.colorInD    = val & 0xF;
                s.colorInC    = (val >> 4) & 0xF;
                s.colorInB    = (val >> 8) & 0xF;
                s.colorInA    = (val >> 12) & 0xF;
                s.colorBias   = (val >> 16) & 0x3;
                s.colorOp     = (val >> 18) & 0x1;
                s.colorClamp  = (val >> 19) & 0x1;
                s.colorScale  = (val >> 20) & 0x3;
                s.colorRegId  = (val >> 22) & 0x3;
            } else {                       
                s.alphaInD    = (val >> 4) & 0x7;
                s.alphaInC    = (val >> 7) & 0x7;
                s.alphaInB    = (val >> 10) & 0x7;
                s.alphaInA    = (val >> 13) & 0x7;
                s.alphaBias   = (val >> 16) & 0x3;
                s.alphaOp     = (val >> 18) & 0x1;
                s.alphaClamp  = (val >> 19) & 0x1;
                s.alphaScale  = (val >> 20) & 0x3;
                s.alphaRegId  = (val >> 22) & 0x3;
            }
        } else if ((reg >= 0x80 && reg <= 0x83) || (reg >= 0xA0 && reg <= 0xA3)) {
            const int idx = reg >= 0xA0 ? reg - 0xA0 + 4 : reg - 0x80;
            g_state.texStages[idx].wrapS = val & 3;
            g_state.texStages[idx].wrapT = (val >> 2) & 3;
        } else if ((reg >= 0x88 && reg <= 0x8B) || (reg >= 0xA8 && reg <= 0xAB)) {
            
            // 10-19, format bits 20-23 (Dolphin BPMemory TexImage0).
            int idx = (reg >= 0x88 && reg <= 0x8B) ? (reg - 0x88) : (reg - 0xA8 + 4);
            if (idx < (int)g_state.texStages.size()) {
                g_state.texStages[idx].width  = ((val >>  0) & 0x3FF) + 1;
                g_state.texStages[idx].height = ((val >> 10) & 0x3FF) + 1;
                g_state.texStages[idx].format = (val >> 20) & 0xF;
            }
        } else if ((reg >= 0x94 && reg <= 0x97) || (reg >= 0xB4 && reg <= 0xB7)) {
            
            int idx = (reg >= 0x94 && reg <= 0x97) ? (reg - 0x94) : (reg - 0xB4 + 4);
            if (idx < (int)g_state.texStages.size()) {
                g_state.texStages[idx].base_addr = (val & 0xFFFFFF) << 5;
            }
        } else if ((reg >= 0x98 && reg <= 0x9F) || (reg >= 0xB8 && reg <= 0xBF)) {

            int idx = (reg >= 0x98 && reg <= 0x9F) ? (reg - 0x98) : (reg - 0xB8 + 4);
            if (idx < (int)g_state.texStages.size()) {
                g_state.texStages[idx].tlut_offset = (val & 0x3FF) << 9;
                g_state.texStages[idx].tlut_format = (val >> 10) & 0x3;
            }
        } else if (reg == 0x64) {

            // (Dolphin names Wind Waker and Double Dash; MP7 does it too, which

            uint32_t addr = (val & 0xFFFFFF) << 5;
            if (nwii::runtime::Config::get().platform ==
                nwii::runtime::Platform::GameCube)
                addr &= 0x01FFFFFF;
            g_state.tlutSrcAddr = addr;
        } else if (reg == 0x65) {

            uint32_t dst = (val & 0x3FF) << 9;
            uint32_t bytes = ((val >> 10) & 0x7FF) * 32;
            if (nwii::runtime::g_mmu && dst + bytes <= sizeof(g_state.tlutMem)) {
                if (const uint8_t* p =
                        nwii::runtime::g_mmu->get_ptr(g_state.tlutSrcAddr)) {
                    std::memcpy(&g_state.tlutMem[dst], p, bytes);
                } else {
                    for (uint32_t i = 0; i < bytes; i++)
                        g_state.tlutMem[dst + i] =
                            nwii::runtime::g_mmu->read8(g_state.tlutSrcAddr + i);
                }
            }
        } else if (reg == 0x40) {
            g_state.zMode.enable  = (val >> 0) & 1;
            g_state.zMode.func    = (val >> 1) & 7;
            g_state.zMode.update  = (val >> 4) & 1;
        } else if (reg == 0xF3) {

            g_state.alphaTest.ref0  = val & 0xFF;
            g_state.alphaTest.ref1  = (val >> 8) & 0xFF;
            g_state.alphaTest.comp0 = (val >> 16) & 0x7;
            g_state.alphaTest.comp1 = (val >> 19) & 0x7;
            g_state.alphaTest.logic = (val >> 22) & 0x3;
        } else if (reg == 0x4F) {
            g_state.clearAR = val & 0xFFFF; 
        } else if (reg == 0x50) {
            g_state.clearGB = val & 0xFFFF; 
        } else if (reg == 0x49) { 
            g_state.efbSrcX = val & 0x3FF;
            g_state.efbSrcY = (val >> 10) & 0x3FF;
        } else if (reg == 0x4A) { 
            g_state.efbW = (val & 0x3FF) + 1;
            g_state.efbH = ((val >> 10) & 0x3FF) + 1;
        } else if (reg == 0x4B) { 
            g_state.efbCopyDest = (val & 0xFFFFFF) << 5;
        } else if (reg == 0x4D) { 
            g_state.efbCopyStride = (val & 0x3FF) << 5;
        } else if (reg == 0x52) { 

            if (gx_trace()) {
                printf("[GXTRACE] BP 0x52 PE_COPY_EXECUTE val=0x%08X (bit14=%d bit11=%d)\n",
                       val, (val & 0x4000) != 0, (val & 0x800) != 0);
            }

            
            if (val & 0x4000) {
                g_state.xfbAddr = g_state.efbCopyDest;
                g_state.xfbW = g_state.efbW;
                g_state.xfbH = g_state.efbH;
                g_state.xfbStride = g_state.efbCopyStride
                                        ? g_state.efbCopyStride
                                        : (uint32_t)g_state.efbW * 2;
                g_state.frame_ready = true;
            }
            if (val & 0x800) {
                g_state.pe_clear_pending = true;
            }
            nwii::runtime::hw::pe_signal_finish();
        }
    }

    

    
    void ParseCP(uint8_t reg, uint32_t val) {
        g_state.cp[reg] = val;
        if (g_skindbg && (reg == 0x50 || reg == 0x30 || reg == 0x60)) {
            static uint32_t last50=0xFFFFFFFF, last30=0xFFFFFFFF, last60=0xFFFFFFFF;
            uint32_t* lp = reg==0x50?&last50:(reg==0x30?&last30:&last60);
            if (val != *lp) {
                *lp = val;
                printf("[CPTR] reg=0x%02X val=0x%08X (0x50.bit0/posMatIdx=%d)\n",
                       reg, val, reg==0x50 ? (val & 1) : -1);
            }
        }
        if (reg >= 0xA0 && reg <= 0xAF) {
            g_state.arrayBase[reg - 0xA0] = val & 0x3FFFFFFF;
        } else if (reg >= 0xB0 && reg <= 0xBF) {
            g_state.arrayStride[reg - 0xB0] = val & 0xFF;
        } else if (reg == 0x30) {
            // Matrix index A: pos/normal (bits 0-5), tex0-3 (6-29).
            if (XfMatIdxShared()) g_parse_xf[0x18] = val; // PEEK10: shared with XF 0x1018 (ParseApplyXF)
            g_state.defPosMtxIdx = val & 0x3F;
            for (int t = 0; t < 4; t++)
                g_state.defTexMtxIdx[t] = (val >> (6 + t * 6)) & 0x3F;
            static const bool tmdbg = std::getenv("NWII_TEXMTXDBG") != nullptr;
            if (tmdbg) {
                static uint32_t seen = 0;
                for (int t = 0; t < 4; t++) seen |= (1u << g_state.defTexMtxIdx[t]);
                static auto t0 = std::chrono::steady_clock::now();
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(now - t0).count() >= 2) {
                    t0 = now;
                    printf("[TEXMTX30] tex0-3 indices:");
                    for (int i = 0; i < 32; i++) if (seen & (1u << i)) printf(" %d", i);
                    printf(" numTexGens=%d\n", g_state.numTexGens);
                    seen = 0;
                }
            }
        } else if (reg == 0x40) {
            // Matrix index B: tex4-7.
            if (XfMatIdxShared()) g_parse_xf[0x19] = val; // PEEK10: shared with XF 0x1019
            for (int t = 0; t < 4; t++)
                g_state.defTexMtxIdx[4 + t] = (val >> (t * 6)) & 0x3F;
            static const bool tmdbg = std::getenv("NWII_TEXMTXDBG") != nullptr;
            if (tmdbg) {
                static uint32_t seen = 0;
                for (int t = 0; t < 8; t++) seen |= (1u << g_state.defTexMtxIdx[t]);
                static auto t0 = std::chrono::steady_clock::now();
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(now - t0).count() >= 2) {
                    t0 = now;
                    printf("[TEXMTX] indices seen:");
                    for (int i = 0; i < 32; i++) if (seen & (1u << i)) printf(" %d", i);
                    printf("\n");
                    seen = 0;
                }
            }
        } else if (reg == 0x50) {

            
            
            for (int i = 0; i < 8; i++) {
                g_state.vat[i].posMatIdx  = (val >> 0) & 1;
                for (int t = 0; t < 8; t++)
                    g_state.vat[i].texMatIdx[t] = (val >> (1 + t)) & 1;
                g_state.vat[i].posMask    = (VtxAttrMask)((val >> 9)  & 3);
                g_state.vat[i].nrmMask    = (VtxAttrMask)((val >> 11) & 3);
                g_state.vat[i].clrMask[0] = (VtxAttrMask)((val >> 13) & 3);
                g_state.vat[i].clrMask[1] = (VtxAttrMask)((val >> 15) & 3);
            }
        } else if (reg == 0x60) {
            
            for (int i = 0; i < 8; i++)
                for (int t = 0; t < 8; t++)
                    g_state.vat[i].texMask[t] = (VtxAttrMask)((val >> (t * 2)) & 3);
        } else if (reg >= 0x70 && reg <= 0x77) {
            
            int i = reg - 0x70;
            g_state.vat[i].posElements  = (val >> 0)  & 1;
            g_state.vat[i].posType      = (VtxAttrType)((val >> 1) & 7);
            g_state.vat[i].posShift     = (val >> 4)  & 0x1F;
            g_state.vat[i].nrmElements  = (val >> 9)  & 1;
            g_state.vat[i].nrmType      = (VtxAttrType)((val >> 10) & 7);
            g_state.vat[i].clrElements[0] = (val >> 13) & 1;
            g_state.vat[i].clrType[0]   = (VtxAttrType)((val >> 14) & 7);
            g_state.vat[i].clrElements[1] = (val >> 17) & 1;
            g_state.vat[i].clrType[1]   = (VtxAttrType)((val >> 18) & 7);
            g_state.vat[i].texElements[0] = (val >> 21) & 1;
            g_state.vat[i].texType[0]   = (VtxAttrType)((val >> 22) & 7);
            g_state.vat[i].texShift[0]  = (val >> 25) & 0x1F;
            // NormalIndex3 is VAT group 0 (CP 0x70+n) bit 31, per VAT slot
            // (Dolphin UVAT_group0; bit 30 is ByteDequant). VCD_LO (CP 0x50)
            // has no such bit.
            g_state.vat[i].nrmIndex3    = ((val >> 31) & 1) != 0;
        } else if (reg >= 0x80 && reg <= 0x87) {
            
            int i = reg - 0x80;
            g_state.vat[i].texElements[1] = (val >> 0)  & 1;
            g_state.vat[i].texType[1]   = (VtxAttrType)((val >> 1) & 7);
            g_state.vat[i].texShift[1]  = (val >> 4)  & 0x1F;
            g_state.vat[i].texElements[2] = (val >> 9)  & 1;
            g_state.vat[i].texType[2]   = (VtxAttrType)((val >> 10) & 7);
            g_state.vat[i].texShift[2]  = (val >> 13) & 0x1F;
            g_state.vat[i].texElements[3] = (val >> 18) & 1;
            g_state.vat[i].texType[3]   = (VtxAttrType)((val >> 19) & 7);
            g_state.vat[i].texShift[3]  = (val >> 22) & 0x1F;
            g_state.vat[i].texElements[4] = (val >> 27) & 1;
            g_state.vat[i].texType[4]   = (VtxAttrType)((val >> 28) & 7);
        } else if (reg >= 0x90 && reg <= 0x97) {
            
            int i = reg - 0x90;
            g_state.vat[i].texShift[4]  = (val >> 0)  & 0x1F;
            g_state.vat[i].texElements[5] = (val >> 5)  & 1;
            g_state.vat[i].texType[5]   = (VtxAttrType)((val >> 6) & 7);
            g_state.vat[i].texShift[5]  = (val >> 9)  & 0x1F;
            g_state.vat[i].texElements[6] = (val >> 14) & 1;
            g_state.vat[i].texType[6]   = (VtxAttrType)((val >> 15) & 7);
            g_state.vat[i].texShift[6]  = (val >> 18) & 0x1F;
            g_state.vat[i].texElements[7] = (val >> 23) & 1;
            g_state.vat[i].texType[7]   = (VtxAttrType)((val >> 24) & 7);
            g_state.vat[i].texShift[7]  = (val >> 27) & 0x1F;
        }
    }

    inline int TypeSize(VtxAttrType type) {
        switch (type) {
            case VtxAttrType::U8: case VtxAttrType::S8: return 1;
            case VtxAttrType::U16: case VtxAttrType::S16: return 2;
            default: return 4; 
        }
    }

    inline float DecodeScalar(const uint8_t* p, VtxAttrType type, uint8_t shift) {
        switch (type) {
            case VtxAttrType::U8:  return (float)p[0] / (float)(1 << shift);
            case VtxAttrType::S8:  return (float)(int8_t)p[0] / (float)(1 << shift);
            case VtxAttrType::U16: return (float)(uint16_t)((p[0] << 8) | p[1]) / (float)(1 << shift);
            case VtxAttrType::S16: return (float)(int16_t)((p[0] << 8) | p[1]) / (float)(1 << shift);
            default: {
                uint32_t v = ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
                float f; std::memcpy(&f, &v, 4); return f;
            }
        }
    }

    

    
    bool ReadVectorAttr(const std::vector<uint8_t>& fifo, size_t& fifo_offset, size_t fifo_size,
                        VtxAttrMask mask, VtxAttrType type, uint8_t shift,
                        uint32_t array_base, uint32_t array_stride, int ncomp, float* out) {
        for (int i = 0; i < ncomp; i++) out[i] = 0.0f;
        if (mask == VtxAttrMask::None) return true;

        int sz = TypeSize(type);
        if (mask == VtxAttrMask::Direct) {
            if (fifo_offset + (size_t)sz * ncomp > fifo_size) return false;
            for (int i = 0; i < ncomp; i++)
                out[i] = DecodeScalar(&fifo[fifo_offset + (size_t)sz * i], type, shift);
            fifo_offset += (size_t)sz * ncomp;
            return true;
        }

        uint32_t index = 0;
        if (mask == VtxAttrMask::Index8) {
            if (fifo_offset + 1 > fifo_size) return false;
            index = fifo[fifo_offset++];
        } else { 
            if (fifo_offset + 2 > fifo_size) return false;
            index = (fifo[fifo_offset] << 8) | fifo[fifo_offset + 1];
            fifo_offset += 2;
        }
        if (!nwii::runtime::g_mmu) return true;
        uint32_t base = array_base + index * array_stride;

        

        const uint8_t* p = nwii::runtime::g_mmu->get_ptr(base);
        if (!p) return true;
        float inv = 1.0f / (float)(1 << shift);
        for (int i = 0; i < ncomp; i++) {
            const uint8_t* q = p + (size_t)sz * i;
            switch (type) {
                case VtxAttrType::U8:  out[i] = (float)q[0] * inv; break;
                case VtxAttrType::S8:  out[i] = (float)(int8_t)q[0] * inv; break;
                case VtxAttrType::U16: out[i] = (float)(uint16_t)((q[0] << 8) | q[1]) * inv; break;
                case VtxAttrType::S16: out[i] = (float)(int16_t)((q[0] << 8) | q[1]) * inv; break;
                default: {
                    uint32_t w = ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) |
                                 ((uint32_t)q[2] << 8) | q[3];
                    std::memcpy(&out[i], &w, 4);
                    break;
                }
            }
        }
        return true;
    }

    
    
    int ColorBytes(VtxAttrType clrType) {
        switch ((int)clrType) {
            case 0: case 3: return 2;
            case 1: case 4: return 3;
            default:        return 4; 
        }
    }

    void DecodeColor(const uint8_t* p, VtxAttrType clrType, float out[4]) {
        switch ((int)clrType) {
            case 0: { 
                uint16_t c = (p[0] << 8) | p[1];
                out[0] = ((c >> 11) & 0x1F) / 31.0f;
                out[1] = ((c >> 5) & 0x3F) / 63.0f;
                out[2] = (c & 0x1F) / 31.0f;
                out[3] = 1.0f;
                break;
            }
            case 1: 
            case 2: 
                out[0] = p[0] / 255.0f;
                out[1] = p[1] / 255.0f;
                out[2] = p[2] / 255.0f;
                out[3] = 1.0f;
                break;
            case 3: { 
                uint16_t c = (p[0] << 8) | p[1];
                out[0] = ((c >> 12) & 0xF) / 15.0f;
                out[1] = ((c >> 8) & 0xF) / 15.0f;
                out[2] = ((c >> 4) & 0xF) / 15.0f;
                out[3] = (c & 0xF) / 15.0f;
                break;
            }
            case 4: { 
                uint32_t c = (p[0] << 16) | (p[1] << 8) | p[2];
                out[0] = ((c >> 18) & 0x3F) / 63.0f;
                out[1] = ((c >> 12) & 0x3F) / 63.0f;
                out[2] = ((c >> 6) & 0x3F) / 63.0f;
                out[3] = (c & 0x3F) / 63.0f;
                break;
            }
            default: 
                out[0] = p[0] / 255.0f;
                out[1] = p[1] / 255.0f;
                out[2] = p[2] / 255.0f;
                out[3] = p[3] / 255.0f;
                break;
        }
    }

    
    bool ReadColorAttr(const std::vector<uint8_t>& fifo, size_t& fifo_offset, size_t fifo_size,
                       VtxAttrMask mask, VtxAttrType clrType,
                       uint32_t array_base, uint32_t array_stride, float out[4]) {
        out[0] = out[1] = out[2] = out[3] = 1.0f;
        if (mask == VtxAttrMask::None) return true;

        int nbytes = ColorBytes(clrType);
        if (mask == VtxAttrMask::Direct) {
            if (fifo_offset + (size_t)nbytes > fifo_size) return false;
            DecodeColor(&fifo[fifo_offset], clrType, out);
            fifo_offset += nbytes;
            return true;
        }

        uint32_t index = 0;
        if (mask == VtxAttrMask::Index8) {
            if (fifo_offset + 1 > fifo_size) return false;
            index = fifo[fifo_offset++];
        } else {
            if (fifo_offset + 2 > fifo_size) return false;
            index = (fifo[fifo_offset] << 8) | fifo[fifo_offset + 1];
            fifo_offset += 2;
        }
        if (!nwii::runtime::g_mmu) return true;
        uint32_t base = array_base + index * array_stride;
        const uint8_t* p = nwii::runtime::g_mmu->get_ptr(base);
        if (!p) return true;
        DecodeColor(p, clrType, out);
        return true;
    }
}


void ApplyBPRegister(uint8_t reg, uint32_t val) { ApplyBPRegisterImpl(reg, val); }

static void ParseStream(const std::vector<uint8_t>& fifo, size_t& offset, std::vector<GXCommand>& commands, int depth);



// Display lists are static geometry the game CALLs every frame; re-parsing
// them dominated the 3D-scene drain (measured 1.3s per 2s). Cache the parsed
// commands keyed by address+content, guarded by a hash of the parser state
// that gets baked into draw snapshots (VAT, arrays, matrix indices, channel
// control) so a state change forces a re-parse. LOAD_INDX payloads are the
// one thing read from RAM at parse time — on a cache hit they are re-read so
// skinning animation stays live. NWII_NODLCACHE=1 disables.
struct DLCacheEntry {
    uint64_t state_hash;
    uint64_t content_hash;
    std::vector<GXCommand> cmds;
};
static std::unordered_map<uint64_t, DLCacheEntry> s_dl_cache;
uint64_t g_prof_dl_hits = 0;
// NWII_SKINDBG report (state declared earlier for ParseCP visibility).
static void skin_report() {
    static auto t0 = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(now - t0).count() < 2) return;
    t0 = now;
    printf("[SKIN] loaded rows:");
    for (int i = 0; i < 64; i++) if (g_skin_loaded_rows & (1ull<<i)) printf(" %d", i);
    printf(" | used posMtxIdx:");
    for (int i = 0; i < 64; i++) if (g_skin_used_rows & (1ull<<i)) printf(" %d", i);
    printf(" | used-but-unloaded:");
    for (int i = 0; i < 64; i++) if (g_skin_used_missing & (1ull<<i)) printf(" %d", i);
    printf(" | draws=%llu perVtxIdx=%llu defNonZero=%llu snapped=%llu\n",
           (unsigned long long)g_skin_draws_total,
           (unsigned long long)g_skin_draws_pmidx,
           (unsigned long long)g_skin_defnz,
           (unsigned long long)g_skin_snapped);
    g_skin_loaded_rows = g_skin_used_rows = g_skin_used_missing = 0;
    g_skin_draws_total = g_skin_draws_pmidx = g_skin_defnz = g_skin_snapped = 0;
}

// M1.8: a texgen with source row NORMAL / BINORMAL_T / BINORMAL_B (XF 0x1040+i
// bits 7-11 = 1, 3, 4) among the active ones (XF 0x103F) reads the vertex
// normal, so the decode must keep it even for unlit draws (EGG light textures).
static bool TexGenReadsNormal() {
    const uint32_t n = std::min<uint32_t>(g_parse_xf[0x03F] & 0xF, 8);
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t row = (g_parse_xf[0x040 + i] >> 7) & 0x1F;
        if (row == 1 || row == 3 || row == 4)
            return true;
    }
    return false;
}

static uint64_t DLStateHash() {
    uint64_t h = 14695981039346656037ull;
    for (int r = 0x30; r <= 0xBF; ++r) { h ^= g_state.cp[r]; h *= 1099511628211ull; }
    // Parse-time XF shadow (M1.2): the render-time g_state.xf lags the parser.
    h ^= g_parse_xf[0x00E]; h *= 1099511628211ull;
    h ^= g_parse_xf[0x00F]; h *= 1099511628211ull;
    h ^= g_parse_xf[0x010]; h *= 1099511628211ull;
    h ^= g_parse_xf[0x011]; h *= 1099511628211ull;
    // need_normal also depends on the texgen source rows (M1.8).
    h ^= TexGenReadsNormal() ? 0x5A5Au : 0u; h *= 1099511628211ull;
    return h;
}

// LIGHT17: content hash over EVERY byte of a display list (XXH64, seed 0).
// The old sampled hash (every 512th byte + the last 32) missed g3d's in-place
// patches of animated TEV colours inside small material lists (title sky DL:
// C3 at +40 of 128 bytes) and served stale register values (REF15: title10
// f780 17.1 dB vs Dolphin). ~10 GB/s; SS calls ~1.3 MB of lists per frame.
static inline uint64_t DLRotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
static inline uint64_t DLRound(uint64_t acc, uint64_t in) {
    acc += in * 0xC2B2AE3D27D4EB4Full;
    return DLRotl(acc, 31) * 0x9E3779B185EBCA87ull;
}
static inline uint64_t DLMerge(uint64_t h, uint64_t v) {
    h ^= DLRound(0, v);
    return h * 0x9E3779B185EBCA87ull + 0x85EBCA77C2B2AE63ull;
}
static uint64_t DLContentHash(const uint8_t* p, uint32_t n) {
    constexpr uint64_t P1 = 0x9E3779B185EBCA87ull, P2 = 0xC2B2AE3D27D4EB4Full,
                       P3 = 0x165667B19E3779F9ull, P4 = 0x85EBCA77C2B2AE63ull,
                       P5 = 0x27D4EB2F165667C5ull;
    uint32_t i = 0;
    uint64_t h;
    if (n >= 32) {
        uint64_t v1 = P1 + P2, v2 = P2, v3 = 0, v4 = 0ull - P1;
        for (; i + 32 <= n; i += 32) {
            uint64_t w[4];
            std::memcpy(w, p + i, 32);
            v1 = DLRound(v1, w[0]); v2 = DLRound(v2, w[1]);
            v3 = DLRound(v3, w[2]); v4 = DLRound(v4, w[3]);
        }
        h = DLRotl(v1, 1) + DLRotl(v2, 7) + DLRotl(v3, 12) + DLRotl(v4, 18);
        h = DLMerge(h, v1); h = DLMerge(h, v2); h = DLMerge(h, v3); h = DLMerge(h, v4);
    } else {
        h = P5;
    }
    h += n;
    for (; i + 8 <= n; i += 8) {
        uint64_t k;
        std::memcpy(&k, p + i, 8);
        h ^= DLRound(0, k);
        h = DLRotl(h, 27) * P1 + P4;
    }
    if (i + 4 <= n) {
        uint32_t k;
        std::memcpy(&k, p + i, 4);
        h ^= (uint64_t)k * P1;
        h = DLRotl(h, 23) * P2 + P3;
        i += 4;
    }
    for (; i < n; ++i) {
        h ^= p[i] * P5;
        h = DLRotl(h, 11) * P1;
    }
    h ^= h >> 33; h *= P2; h ^= h >> 29; h *= P3; h ^= h >> 32;
    return h;
}

static void ExpandDisplayList(uint32_t addr, uint32_t size,
                              std::vector<GXCommand>& commands, int depth) {
    uint32_t phys = addr & 0x1FFFFFFF;
    // GX fetches display lists from MEM1 (phys 0-0x017FFFFF) and MEM2 (phys
    // 0x10000000-0x13FFFFFF). SS keeps its g3d material/shape lists (the F000
    // sky, clouds and island behind Title/FileSelect) in MEM2; rejecting MEM2
    // dropped ~91% of all DL calls (M1.0-diagnosis section 1).
    const bool in_mem1 = phys < 0x01800000u && size <= 0x01800000u - phys;
    const bool in_mem2 = phys >= 0x10000000u && phys < 0x14000000u &&
                         size <= 0x14000000u - phys;
    if (!nwii::runtime::g_mmu || size == 0 || size > 0x400000 ||
        !(in_mem1 || in_mem2) || depth >= 4)
        return;
    g_prof_dl_calls++;
    g_prof_dl_bytes += size;
    auto dl0 = std::chrono::steady_clock::now();

    static const bool nocache = std::getenv("NWII_NODLCACHE") != nullptr;
    const uint8_t* src = nwii::runtime::g_mmu->get_ptr(0x80000000u | phys, size);
    // Prefer the copy taken when the game actually made the call -- guest RAM
    // may already hold the next frame's list by the time we get here.
    std::vector<uint8_t> snap;
    if (nwii::runtime::GX_DL_Fetch(addr, size, snap)) src = snap.data();
    if (g_capNoteDL && src) g_capNoteDL(addr, size, src); // M0.4 capture journal

    if (src && !nocache && depth == 0) {
        // LIGHT17: exact content hash (every byte). The game patches small
        // material lists in place (animated TEV colours), so a sampled hash
        // is not enough. NWII_NODLCACHE=1 stays the A/B (no cache at all).
        const uint64_t ch = DLContentHash(src, size);
        // Keyed by list AND parser state: g3d calls the same material/shape
        // lists under several VAT/array states per frame, and a key without
        // the state made those calls evict each other (cache hit rate ~12%
        // once MEM2 lists were accepted).
        uint64_t sh = DLStateHash();
        uint64_t key = (((uint64_t)phys << 23) ^ size) * 0x9E3779B97F4A7C15ull ^ sh;
        auto it = s_dl_cache.find(key);
        if (it != s_dl_cache.end() && it->second.state_hash == sh &&
            it->second.content_hash == ch) {
            g_prof_dl_hits++;
            for (const GXCommand& c0 : it->second.cmds) {
                commands.push_back(c0);
                GXCommand& c = commands.back();
                // Replay parser-state side effects so main-stream commands
                // after the CALL still see the DL's CP/BP writes.
                if (c.type == GXCommandType::CPRegister)
                    ParseCP((uint8_t)c.reg, c.val);
                else if (c.type == GXCommandType::BPRegister)
                    ParseBP((uint8_t)c.reg, c.val);
                else if (c.type == GXCommandType::XFRegister &&
                         (c.val & 0x80000000u)) {
                    // LOAD_INDX marker: re-read the (possibly animated)
                    // matrix data from guest RAM at today's array base.
                    int array = 12 + ((c.val >> 24) & 0x3);
                    int num = (c.val >> 16) & 0xFF;
                    uint32_t index = c.val & 0xFFFF;
                    uint32_t base = g_state.arrayBase[array] +
                                    index * g_state.arrayStride[array];
                    for (int i = 0; i < num && i < (int)c.payload.size(); i++)
                        c.payload[i] =
                            nwii::runtime::g_mmu->read_f32(base + i * 4);
                    if (g_capNoteRAMFloats) // M0.4 capture journal
                        g_capNoteRAMFloats(base, c.payload.data(),
                                           std::min(num, (int)c.payload.size()));
                    if (c.reg < 256 || (c.reg >= 0x400 && c.reg < 0x460))
                        ParseApplyMatrixWrite(c.reg, c.payload);
                } else if (c.type == GXCommandType::XFRegister) {
                    // Direct XF write cached in the DL: feed the parse shadows
                    // (matrices for re-snapshots below, XF regs for
                    // need_normal and the state hash).
                    ParseApplyXF(c.reg, c.payload);
                } else if (c.type == GXCommandType::DrawPrimitive && c.raw &&
                           c.raw->have_snap_mtx) {
                    // Re-snapshot the matrix from the current parse shadow so a
                    // cached skinned draw animates instead of freezing at the
                    // pose from when it was first parsed. (raw is shared_ptr;
                    // copy before mutating so the cache entry stays pristine.)
                    // RENDER1: static scenery re-snapshots the same matrix
                    // every frame; copying the DrawRaw (vertex bytes and
                    // all) only when the matrix really changed removed most
                    // of the cache-hit cost once MEM2 model lists were in.
                    uint32_t pb = (uint32_t)c.raw->defPosMtxIdx * 4;
                    uint32_t nb = 3u * (c.raw->defPosMtxIdx & 31);
                    if (pb + 12 <= 256 && nb + 9 <= 96 &&
                        (std::memcmp(c.raw->snap_pos_mtx, &g_parse_posmtx[pb], 12 * sizeof(float)) != 0 ||
                         std::memcmp(c.raw->snap_nrm_mtx, &g_parse_nrmmtx[nb], 9 * sizeof(float)) != 0)) {
                        c.raw = std::make_shared<DrawRaw>(*c.raw);
                        std::memcpy(c.raw->snap_pos_mtx, &g_parse_posmtx[pb], 12 * sizeof(float));
                        std::memcpy(c.raw->snap_nrm_mtx, &g_parse_nrmmtx[nb], 9 * sizeof(float));
                    }
                } else if (c.type == GXCommandType::DrawPrimitive && c.raw &&
                           c.raw->vat.posMatIdx) {
                    // TEX7: per-vertex matrix index (g3d skinned/envelope
                    // shapes) freezes the whole bank at parse time. A cache
                    // hit must re-freeze it from today's shadow (the
                    // PrePrim DL's LOAD_INDX just re-read the matrices), or
                    // skinned bodies keep the pose AND camera of the frame
                    // the list was first parsed -- CAP7 D2: Link's/Zelda's
                    // bodies drawn 800-2600 units away from head and hands.
                    bool any = false;
                    for (int k = 0; k < 256 && !any; ++k) any = g_parse_posmtx[k] != 0.0f;
                    const DrawRaw& o = *c.raw;
                    const bool same =
                        any ? (o.snap_pos_bank.size() == 256 && o.snap_nrm_bank.size() == 96 &&
                               std::memcmp(o.snap_pos_bank.data(), g_parse_posmtx, sizeof(g_parse_posmtx)) == 0 &&
                               std::memcmp(o.snap_nrm_bank.data(), g_parse_nrmmtx, sizeof(g_parse_nrmmtx)) == 0)
                            : o.snap_pos_bank.empty();
                    if (!same) {
                        c.raw = std::make_shared<DrawRaw>(o);
                        if (any) {
                            c.raw->snap_pos_bank.assign(g_parse_posmtx, g_parse_posmtx + 256);
                            c.raw->snap_nrm_bank.assign(g_parse_nrmmtx, g_parse_nrmmtx + 96);
                        } else {
                            c.raw->snap_pos_bank.clear();
                            c.raw->snap_nrm_bank.clear();
                        }
                    }
                }
            }
            auto dl1 = std::chrono::steady_clock::now();
            g_prof_dl_us += std::chrono::duration_cast<std::chrono::microseconds>(dl1 - dl0).count();
            return;
        }
        // Miss: parse into a local vector so it can be stored.
        std::vector<uint8_t> dl(src, src + size);
        size_t off = 0;
        std::vector<GXCommand> local;
        ParseStream(dl, off, local, depth + 1);
        // SS calls ~1000 distinct lists per frame once MEM2 lists are
        // accepted; a 512-entry cap would flush the cache every frame.
        if (s_dl_cache.size() > 8192)
            s_dl_cache.clear();
        commands.insert(commands.end(), local.begin(), local.end());
        s_dl_cache[key] = {sh, ch, std::move(local)}; // PERF13: one copy, not two
        auto dl1 = std::chrono::steady_clock::now();
        g_prof_dl_us += std::chrono::duration_cast<std::chrono::microseconds>(dl1 - dl0).count();
        return;
    }

    std::vector<uint8_t> dl(size);
    if (src) {
        std::memcpy(dl.data(), src, size);
    } else {
        for (uint32_t i = 0; i < size; i++)
            dl[i] = nwii::runtime::g_mmu->read8(addr + i);
        if (g_capNoteDL) g_capNoteDL(addr, size, dl.data()); // M0.4 capture journal
    }
    size_t off = 0;
    ParseStream(dl, off, commands, depth + 1);
    auto dl1 = std::chrono::steady_clock::now();
    g_prof_dl_us += std::chrono::duration_cast<std::chrono::microseconds>(dl1 - dl0).count();
}

// Drain-path profiling counters, reported by ProcessGXFifo's [GXPROF] line.
// The 3D-scene drain still burns seconds somewhere after deferred decode;
// these split the cost by cause instead of guessing.
uint64_t g_prof_draws = 0;        // draws snapshotted
uint64_t g_prof_draw_bytes = 0;   // raw vertex bytes copied
uint64_t g_prof_dl_calls = 0;     // display-list expansions
uint64_t g_prof_dl_bytes = 0;     // display-list bytes copied+parsed
uint64_t g_prof_cmds = 0;         // commands emitted
uint64_t g_prof_unknown = 0;
uint64_t g_prof_dl_us = 0;      // time inside display-list expansion
uint64_t g_prof_snap_us = 0;    // time snapshotting draws
uint64_t g_prof_dl_loadindx = 0; // LOAD_INDX commands seen inside display lists      // unknown-opcode single-byte skips

namespace {
    // Byte width of one attribute in the vertex stream.
    inline size_t AttrBytes(VtxAttrMask mask, VtxAttrType type, int ncomp,
                            bool is_color) {
        switch (mask) {
        case VtxAttrMask::None:    return 0;
        case VtxAttrMask::Index8:  return 1;
        case VtxAttrMask::Index16: return 2;
        default:
            return is_color ? (size_t)ColorBytes(type)
                            : (size_t)TypeSize(type) * ncomp;
        }
    }

    size_t VertexSize(const VATSlot& vat) {
        size_t sz = vat.posMatIdx ? 1 : 0;
        for (int t = 0; t < 8; t++)
            if (vat.texMatIdx[t]) sz++;
        sz += AttrBytes(vat.posMask, vat.posType, vat.posElements ? 3 : 2, false);
        {
            size_t nrm = AttrBytes(vat.nrmMask, vat.nrmType,
                                   vat.nrmElements ? 9 : 3, false);
            // Three indices, not one, when NBT normals come in by index.
            if (vat.nrmIndex3 && vat.nrmElements &&
                (vat.nrmMask == VtxAttrMask::Index8 ||
                 vat.nrmMask == VtxAttrMask::Index16))
                nrm *= 3;
            sz += nrm;
        }
        for (int ci = 0; ci < 2; ci++)
            sz += AttrBytes(vat.clrMask[ci], vat.clrType[ci], 1, true);
        for (int t = 0; t < 8; t++)
            sz += AttrBytes(vat.texMask[t], vat.texType[t],
                            vat.texElements[t] ? 2 : 1, false);
        return sz;
    }
} // namespace

void FifoParser::DecodeDraw(GXCommand& c) {
    if (c.type != GXCommandType::DrawPrimitive || !c.raw || !c.vertices.empty())
        return;
    const DrawRaw& r = *c.raw;
    const std::vector<uint8_t>& buf = r.bytes;
    const size_t buf_size = buf.size();
    size_t off = 0;
    c.vertices.reserve(r.count);
    for (uint32_t i = 0; i < r.count; i++) {
        VertexData vtx;
        // GX_POS_XY supplies x and y only; the third component is 0. Left
        // uninitialised it made the decode-time matrix transform produce NaN
        // (EGG post-effect quads and lyt panes vanished; M1.0 section 6).
        vtx.pos[0] = vtx.pos[1] = vtx.pos[2] = 0.0f;
        vtx.posMtxIdx = r.defPosMtxIdx;
        for (int t = 0; t < 8; t++)
            vtx.texMtxIdx[t] = r.defTexMtxIdx[t];
        if (r.vat.posMatIdx) {
            if (off + 1 > buf_size) return;
            vtx.posMtxIdx = buf[off++];
        }
        if (g_skindbg && vtx.posMtxIdx < 64) {
            g_skin_used_rows |= (1ull << vtx.posMtxIdx);
            if (!(g_skin_loaded_rows & (1ull << vtx.posMtxIdx)))
                g_skin_used_missing |= (1ull << vtx.posMtxIdx);
        }
        for (int t = 0; t < 8; t++) {
            if (r.vat.texMatIdx[t]) {
                if (off + 1 > buf_size) return;
                vtx.texMtxIdx[t] = buf[off++];
            }
        }
        vtx.has_pos = (r.vat.posMask != VtxAttrMask::None);
        if (!ReadVectorAttr(buf, off, buf_size, r.vat.posMask, r.vat.posType,
                            r.vat.posShift, r.arrayBase[0], r.arrayStride[0],
                            r.vat.posElements ? 3 : 2, vtx.pos))
            return;
        // CLR1 and tex1-7 are parsed for stream position only (the renderer
        // uses clr0/tex0). Normals are decoded only for lit draws.
        // Three indices when NBT normals arrive by index: consume the extra two
        // so the stream position stays right even though only the normal is
        // used. Getting this wrong is invisible on this vertex and fatal on
        // every one after it.
        const bool nrm_idx3 =
            r.vat.nrmIndex3 && r.vat.nrmElements &&
            (r.vat.nrmMask == VtxAttrMask::Index8 ||
             r.vat.nrmMask == VtxAttrMask::Index16);
        const size_t nrm_idx_bytes =
            (r.vat.nrmMask == VtxAttrMask::Index16) ? 2 : 1;

        if (r.vat.nrmMask != VtxAttrMask::None && r.need_normal) {
            vtx.has_norm = true;
            float nbt[9];
            // TEX7: normals ignore the VAT frac field; the hardware uses a fixed
            // scale per type (Dolphin VertexLoader_Normal FracAdjust): s8 2^-6,
            // u8 2^-7, s16 2^-14, u16 2^-15. Shift 0 left g3d's s16 normals at
            // length ~16384, which the NORMAL texgen (EGG light textures) feeds
            // straight into the texture matrix.
            const uint8_t nrm_shift =
                r.vat.nrmType == VtxAttrType::S8  ? 6 :
                r.vat.nrmType == VtxAttrType::U8  ? 7 :
                r.vat.nrmType == VtxAttrType::S16 ? 14 :
                r.vat.nrmType == VtxAttrType::U16 ? 15 : 0;
            if (!ReadVectorAttr(buf, off, buf_size, r.vat.nrmMask, r.vat.nrmType,
                                nrm_shift, r.arrayBase[1], r.arrayStride[1],
                                r.vat.nrmElements ? 9 : 3, nbt))
                return;
            if (nrm_idx3) off += 2 * nrm_idx_bytes;
            vtx.norm[0] = nbt[0]; vtx.norm[1] = nbt[1]; vtx.norm[2] = nbt[2];
        } else {
            off += AttrBytes(r.vat.nrmMask, r.vat.nrmType,
                             r.vat.nrmElements ? 9 : 3, false);
            if (nrm_idx3) off += 2 * nrm_idx_bytes;
        }
        if (r.vat.clrMask[0] != VtxAttrMask::None) {
            float col[4];
            if (!ReadColorAttr(buf, off, buf_size, r.vat.clrMask[0],
                               r.vat.clrType[0], r.arrayBase[2],
                               r.arrayStride[2], col))
                return;
            vtx.has_color = true;
            vtx.color[0] = col[0]; vtx.color[1] = col[1];
            vtx.color[2] = col[2]; vtx.color[3] = col[3];
        }
        off += AttrBytes(r.vat.clrMask[1], r.vat.clrType[1], 1, true);
        // All eight texcoords: XF texgen (M1.8) may read any of them.
        for (int t = 0; t < 8; t++) {
            if (r.vat.texMask[t] == VtxAttrMask::None)
                continue;
            vtx.has_tex[t] = true;
            vtx.tex[t][1] = 0.0f; // one-element (S only) coords leave T at 0
            if (!ReadVectorAttr(buf, off, buf_size, r.vat.texMask[t],
                                r.vat.texType[t], r.vat.texShift[t],
                                r.arrayBase[4 + t], r.arrayStride[4 + t],
                                r.vat.texElements[t] ? 2 : 1, vtx.tex[t]))
                return;
        }
        if (off > buf_size)
            return;
        // Model-space inputs of the XF texgen (source rows GEOM / NORMAL),
        // taken before the decode-time transform below.
        std::memcpy(vtx.raw_pos, vtx.pos, sizeof vtx.raw_pos);
        if (vtx.has_norm)
            std::memcpy(vtx.raw_norm, vtx.norm, sizeof vtx.raw_norm);

        // Transform by the per-draw matrix snapshot (GX 3x4). Doing it here,
        // with the matrix captured at this draw's stream position, fixes the
        // matrix-swap skinning scramble where a live render-time lookup would
        // use whatever matrix was written last.
        if (!r.snap_pos_bank.empty() && vtx.has_pos) {
            const uint32_t pb = (uint32_t)vtx.posMtxIdx * 4;
            const uint32_t nb = 3u * (vtx.posMtxIdx & 31);
            if (pb + 12 <= 256 && nb + 9 <= 96) {
                const float* m = &r.snap_pos_bank[pb];
                float x = vtx.pos[0], y = vtx.pos[1], z = vtx.pos[2];
                vtx.pos[0] = m[0]*x + m[1]*y + m[2]*z + m[3];
                vtx.pos[1] = m[4]*x + m[5]*y + m[6]*z + m[7];
                vtx.pos[2] = m[8]*x + m[9]*y + m[10]*z + m[11];
                if (vtx.has_norm && !r.snap_nrm_bank.empty()) {
                    const float* n = &r.snap_nrm_bank[nb];
                    float a = vtx.norm[0], b = vtx.norm[1], cc = vtx.norm[2];
                    vtx.norm[0] = n[0]*a + n[1]*b + n[2]*cc;
                    vtx.norm[1] = n[3]*a + n[4]*b + n[5]*cc;
                    vtx.norm[2] = n[6]*a + n[7]*b + n[8]*cc;
                }
                vtx.pre_xf = true;
            }
        } else if (r.have_snap_mtx && vtx.has_pos) {
            const float* m = r.snap_pos_mtx;
            float x = vtx.pos[0], y = vtx.pos[1], z = vtx.pos[2];
            vtx.pos[0] = m[0]*x + m[1]*y + m[2]*z + m[3];
            vtx.pos[1] = m[4]*x + m[5]*y + m[6]*z + m[7];
            vtx.pos[2] = m[8]*x + m[9]*y + m[10]*z + m[11];
            if (vtx.has_norm) {
                const float* n = r.snap_nrm_mtx;
                float a = vtx.norm[0], b = vtx.norm[1], cc = vtx.norm[2];
                vtx.norm[0] = n[0]*a + n[1]*b + n[2]*cc;
                vtx.norm[1] = n[3]*a + n[4]*b + n[5]*cc;
                vtx.norm[2] = n[6]*a + n[7]*b + n[8]*cc;
            }
            vtx.pre_xf = true;
        }
        c.vertices.push_back(vtx);
    }
    c.raw.reset(); // decoded: drop the raw copy
}

// ---- M0.4 capture/replay accessors (tooling only; no behaviour change) -----
void (*g_capNoteDL)(uint32_t, uint32_t, const uint8_t*) = nullptr;
void (*g_capNoteRAMFloats)(uint32_t, const float*, int) = nullptr;
void ParserShadowGet(float pos[256], float nrm[96]) {
    std::memcpy(pos, g_parse_posmtx, sizeof(g_parse_posmtx));
    std::memcpy(nrm, g_parse_nrmmtx, sizeof(g_parse_nrmmtx));
}
void ParserShadowSet(const float pos[256], const float nrm[96]) {
    std::memcpy(g_parse_posmtx, pos, sizeof(g_parse_posmtx));
    std::memcpy(g_parse_nrmmtx, nrm, sizeof(g_parse_nrmmtx));
}
void ParserDLCacheVisit(DLCacheVisitor fn, void* user) {
    for (const auto& kv : s_dl_cache)
        fn(user, kv.first, kv.second.state_hash, kv.second.content_hash, kv.second.cmds);
}
void ParserDLCacheClear() { s_dl_cache.clear(); }
// RENDER1: parse-only BP/XF shadows and both BP-mask registers. A capture
// stores the render-time g_state; the replay seeds the parse shadows from it.
void ParserRegShadowSet(const uint32_t bp[256], const uint32_t xf[256]) {
    std::memcpy(g_parse_bp, bp, sizeof(g_parse_bp));
    std::memcpy(g_parse_xf, xf, sizeof(g_parse_xf));
    g_parse_bpmask = 0xFFFFFF;
    s_render_bpmask = 0xFFFFFF;
    g_parse_bp[0xFE] = 0xFFFFFF;
}
const uint32_t* ParserBPShadow() { return g_parse_bp; }
const uint32_t* ParserXFShadow() { return g_parse_xf; }
void ParserDLCachePut(uint64_t key, uint64_t state_hash, uint64_t content_hash,
                      std::vector<GXCommand>&& cmds) {
    s_dl_cache[key] = {state_hash, content_hash, std::move(cmds)};
}

void FifoParser::Parse(std::vector<uint8_t>& fifo, std::vector<GXCommand>& commands) {
    static bool skin_init = (g_skindbg = std::getenv("NWII_SKINDBG") != nullptr, true);
    (void)skin_init;
    if (g_skindbg) skin_report();
    size_t offset = 0;
    ParseStream(fifo, offset, commands, 0);

    
    if (offset > 0 && offset <= fifo.size())
        fifo.erase(fifo.begin(), fifo.begin() + offset);
}

static void ParseStream(const std::vector<uint8_t>& fifo, size_t& offset, std::vector<GXCommand>& commands, int depth) {
    const size_t fifo_size = fifo.size();
    while (offset < fifo_size) {
        uint8_t cmd = fifo[offset];

        if (cmd == 0x00) {
            
            offset++;
        } else if (cmd == 0x40) {
            
            if (offset + 9 > fifo_size) break;
            uint32_t dl_addr = Read32(fifo, offset + 1);
            uint32_t dl_size = Read32(fifo, offset + 5);
            // NWII_DLDBG: are we expanding the same lists over and over?
            static const bool dldbg = std::getenv("NWII_DLDBG") != nullptr;
            if (dldbg) {
                static std::map<uint64_t, uint32_t> hits;
                static auto t0 = std::chrono::steady_clock::now();
                hits[((uint64_t)dl_addr << 32) | dl_size]++;
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(now - t0).count() >= 2) {
                    t0 = now;
                    std::vector<std::pair<uint32_t, uint64_t>> top;
                    for (auto &kv : hits) top.push_back({kv.second, kv.first});
                    std::sort(top.rbegin(), top.rend());
                    printf("[DLDBG] distinct=%zu top:", hits.size());
                    for (size_t i = 0; i < top.size() && i < 5; i++)
                        printf(" %08X/%uB x%u", (uint32_t)(top[i].second >> 32),
                               (uint32_t)top[i].second, top[i].first);
                    printf("\n");
                    hits.clear();
                }
            }
            ExpandDisplayList(dl_addr, dl_size, commands, depth);
            offset += 9;
        } else if (cmd == 0x78) {
            // Seam RMW marker (not a real GP opcode). Native GX handlers own
            // only some bits of GEN_MODE / CMODE0 / CP 0x30; merging at call
            // time raced this parser and used a base value up to a frame
            // stale, clobbering the other fields (texgen counts, dither bits,
            // texture matrix indices). The merge happens here instead, at the
            // marker's position in the stream, so the base is always current.
            if (offset + 11 > fifo_size) break;
            uint8_t kind = fifo[offset + 1];
            uint8_t reg  = fifo[offset + 2];
            uint32_t mask = Read32(fifo, offset + 3);
            uint32_t val  = Read32(fifo, offset + 7);
            GXCommand c;
            c.reg = reg;
            if (kind == 0) {
                // Merge base: the parse-time BP shadow, which is exactly the
                // render-time value at this stream position.
                uint32_t merged = ((g_parse_bp[reg] & ~mask) | (val & mask)) & 0xFFFFFF;
                ParseBP(reg, merged);
                c.type = GXCommandType::BPRegister;
                c.val = merged;
            } else {
                uint32_t merged = (g_state.cp[reg] & ~mask) | (val & mask);
                g_state.cp[reg] = merged;
                ParseCP(reg, merged);
                c.type = GXCommandType::CPRegister;
                c.val = merged;
            }
            commands.push_back(std::move(c));
            offset += 11;
        } else if (cmd == 0x48) {
            
            offset++;
        } else if (cmd == 0x20 || cmd == 0x28 || cmd == 0x30 || cmd == 0x38) {
            if (depth > 0) g_prof_dl_loadindx++;
            if (offset + 5 > fifo_size) break;
            uint32_t val = Read32(fifo, offset + 1);
            int array = 12 + (cmd - 0x20) / 8;
            uint32_t index = val >> 16;
            uint32_t xf_addr = val & 0xFFF;
            int num = ((val >> 12) & 0xF) + 1;
            if (nwii::runtime::g_mmu) {
                GXCommand c;
                c.type = GXCommandType::XFRegister;
                c.reg = xf_addr;
                c.length = num - 1;
                // LOAD_INDX marker for the DL cache: array/num/index packed so
                // a cache hit can re-read live matrix data.
                c.val = 0x80000000u | ((uint32_t)(array - 12) << 24) |
                        ((uint32_t)num << 16) | (index & 0xFFFF);
                c.payload.resize(num);
                uint32_t base = g_state.arrayBase[array] +
                                index * g_state.arrayStride[array];
                for (int i = 0; i < num; i++)
                    c.payload[i] = nwii::runtime::g_mmu->read_f32(base + i * 4);
                if (g_capNoteRAMFloats) // M0.4 capture journal
                    g_capNoteRAMFloats(base, c.payload.data(), num);
                if (gx_trace()) {
                    static int ln = 0;
                    if (ln++ < 24) {
                        printf("[GXTRACE] LOAD_INDX arr=%d idx=%u xf=0x%03X n=%d base=0x%X stride=%u [",
                               array, index, xf_addr, num, base,
                               g_state.arrayStride[array]);
                        for (int i = 0; i < num && i < 8; i++)
                            printf("%.3f ", c.payload[i]);
                        printf("]\n");
                    }
                }
                if (g_skindbg && xf_addr < 0x100 && (xf_addr % 4) == 0)
                    g_skin_loaded_rows |= (1ull << (xf_addr / 4));
                if (xf_addr < 256 || (xf_addr >= 0x400 && xf_addr < 0x460))
                    ParseApplyMatrixWrite(xf_addr, c.payload);
                commands.push_back(std::move(c));
            }
            offset += 5;
        } else if (cmd == 0x61) {
            
            if (offset + 5 > fifo_size) break;
            uint8_t reg = fifo[offset + 1];
            uint32_t val = Read24(fifo, offset + 2);
            ParseBP(reg, val);

            GXCommand c;
            c.type = GXCommandType::BPRegister;
            c.reg = reg;
            c.val = val;
            commands.push_back(std::move(c));

            offset += 5;
        } else if (cmd == 0x08) {
            
            if (offset + 6 > fifo_size) break;
            uint8_t reg = fifo[offset + 1];
            uint32_t val = Read32(fifo, offset + 2);
            ParseCP(reg, val);

            GXCommand c;
            c.type = GXCommandType::CPRegister;
            c.reg = reg;
            c.val = val;
            commands.push_back(std::move(c));

            offset += 6;
        } else if (cmd == 0x10) {
            
            if (offset + 5 > fifo_size) break;
            uint16_t length = (fifo[offset + 1] << 8) | fifo[offset + 2];
            uint32_t total_size = 5 + ((length + 1) * 4);
            if (offset + total_size > fifo_size) break;

            GXCommand c;
            c.type = GXCommandType::XFRegister;
            c.length = length;
            c.val = 0; // not a LOAD_INDX marker (the DL cache tests bit 31)
            c.reg = (fifo[offset + 3] << 8) | fifo[offset + 4];

            if (gx_trace() && c.reg >= 0x1020 && c.reg <= 0x1027) {
                static int xf_print_proj = 0;
                if (xf_print_proj++ < 5)
                    printf("[GXTRACE] XF proj load: len=%d reg=0x%04X\n", length, c.reg);
            }
            if (gx_trace() && c.reg < 0x100) {
                static int xf_print_mtx = 0;
                if (xf_print_mtx++ < 24)
                    printf("[GXTRACE] XF direct mtx: reg=0x%03X len=%d\n", c.reg, length + 1);
            }

            // NWII_XFDBG: is the game actually animating the colour-channel
            // material/ambient registers? (0x100a-0x1011). Runs in the parser
            // so it works headless, where the renderer never runs.
            static const bool xfdbg = std::getenv("NWII_XFDBG") != nullptr;
            if (xfdbg && c.reg >= 0x1009 && c.reg <= 0x1011) {
                static uint32_t last[9] = {0};
                static uint32_t changes[9] = {0};
                static uint64_t writes[9] = {0};
                static auto t0 = std::chrono::steady_clock::now();
                for (int i = 0; i <= length; i++) {
                    int r = c.reg + i - 0x1009;
                    if (r < 0 || r > 8) continue;
                    uint32_t v = Read32(fifo, offset + 5 + i * 4);
                    writes[r]++;
                    if (v != last[r]) { changes[r]++; last[r] = v; }
                }
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(now - t0).count() >= 1) {
                    t0 = now;
                    printf("[XFDBG] chan/mat regs (reg: writes/changes last):");
                    static const char* nm[9] = {"numchan","amb0","amb1","mat0","mat1",
                                                "cc0","cc1","ac0","ac1"};
                    for (int r = 0; r < 9; r++)
                        printf(" %s:%llu/%u=%08X", nm[r],
                               (unsigned long long)writes[r], changes[r], last[r]);
                    printf("\n");
                }
            }

            int num_floats = length + 1;
            c.payload.resize(num_floats);
            for (int i = 0; i < num_floats; ++i) {
                uint32_t val = Read32(fifo, offset + 5 + (i * 4));
                std::memcpy(&c.payload[i], &val, 4);
            }

            ParseApplyXF(c.reg, c.payload);

            commands.push_back(std::move(c));

            offset += total_size;
        } else if (cmd >= 0x80 && cmd <= 0xBF) {
            
            if (offset + 3 > fifo_size) break;

            uint16_t vtx_count = (fifo[offset + 1] << 8) | fifo[offset + 2];
            uint8_t vat_idx = cmd & 0x07;

            VATSlot& vat = g_state.vat[vat_idx];

            // Vertex size is constant for a draw (every attribute is either
            // direct with a fixed byte width or a fixed 1/2-byte index), so
            // the payload length is known without touching the vertices.
            // Decode is deferred: frame-skip drops most draws, and decoding
            // only survivors is what keeps parse cost off the drain path.
            size_t vsize = VertexSize(vat);
            size_t need = 3 + (size_t)vtx_count * vsize;
            if (offset + need > fifo_size) break; // incomplete: wait for more

            auto sn0 = std::chrono::steady_clock::now();
            GXCommand c;
            c.type = GXCommandType::DrawPrimitive;
            c.prim_type = cmd & 0xF8;
            c.raw = std::make_shared<DrawRaw>();
            c.raw->vat = vat;
            c.raw->defPosMtxIdx = g_state.defPosMtxIdx;
            std::memcpy(c.raw->defTexMtxIdx, g_state.defTexMtxIdx, 8);
            {
                // PEEK10 diagnostic: CP 0x30/0x40 vs XF 0x1018/0x1019 defaults.
                static const bool mdbg = std::getenv("NWII_MTXIDX_DBG") != nullptr;
                if (mdbg) {
                    static unsigned shown = 0, diff = 0, total = 0;
                    const uint32_t a = g_parse_xf[0x18], b = g_parse_xf[0x19];
                    uint8_t x[9] = {(uint8_t)(a & 0x3F)};
                    for (int t = 0; t < 4; ++t) x[1 + t] = (a >> (6 + 6 * t)) & 0x3F;
                    for (int t = 0; t < 4; ++t) x[5 + t] = (b >> (6 * t)) & 0x3F;
                    bool d = x[0] != g_state.defPosMtxIdx;
                    const uint32_t ntg = g_parse_xf[0x3F] & 0xF;
                    for (uint32_t t = 0; t < ntg && t < 8; ++t)
                        d |= x[1 + t] != g_state.defTexMtxIdx[t];
                    ++total;
                    static unsigned shown_vat = 0;
                    bool pv = false;
                    for (uint32_t t = 0; t < ntg && t < 8; ++t) pv |= vat.texMatIdx[t];
                    if (pv && shown_vat++ < 6)
                        printf("[MTXIDX] draw %u: per-vertex texmtx idx (vat) pnmtx=%d tex=%d%d%d%d ntg=%u xf18=%08X\n",
                               total, vat.posMatIdx ? 1 : 0, vat.texMatIdx[0] ? 1 : 0,
                               vat.texMatIdx[1] ? 1 : 0, vat.texMatIdx[2] ? 1 : 0,
                               vat.texMatIdx[3] ? 1 : 0, ntg, a);
                    if (d) {
                        ++diff;
                        if (shown++ < 12)
                            printf("[MTXIDX] draw %u: cp pos=%u tex=%u,%u,%u,%u xf pos=%u tex=%u,%u,%u,%u ntg=%u\n",
                                   total, g_state.defPosMtxIdx, g_state.defTexMtxIdx[0],
                                   g_state.defTexMtxIdx[1], g_state.defTexMtxIdx[2],
                                   g_state.defTexMtxIdx[3], x[0], x[1], x[2], x[3], x[4], ntg);
                    }
                    if ((total & 4095) == 0)
                        printf("[MTXIDX] %u draws, %u with CP != XF defaults\n", total, diff);
                }
            }
            // Snapshot this draw's position/normal matrix from the parse-time
            // shadow, unless the draw carries a per-vertex matrix index (then
            // each vertex may use a different matrix — fall back to live).
            if (vat.posMatIdx) {
                // Per-vertex matrix index: freeze the entire bank, because the
                // vertices in this one draw will index different slots and the
                // renderer's live copy is whatever the frame ended up with.
                bool any = false;
                for (int k = 0; k < 256 && !any; ++k) any = g_parse_posmtx[k] != 0.0f;
                if (any) {
                    c.raw->snap_pos_bank.assign(g_parse_posmtx, g_parse_posmtx + 256);
                    c.raw->snap_nrm_bank.assign(g_parse_nrmmtx, g_parse_nrmmtx + 96);
                }
            } else {
                uint32_t pb = (uint32_t)g_state.defPosMtxIdx * 4;
                uint32_t nb = 3u * (g_state.defPosMtxIdx & 31);
                if (pb + 12 <= 256 && nb + 9 <= 96) {
                    const float* pm = &g_parse_posmtx[pb];
                    // Only trust the snapshot if the slot was actually written
                    // (a zero matrix = never loaded in the parse shadow) — else
                    // fall back to the renderer's live matrix.
                    bool nonzero = false;
                    for (int k = 0; k < 12 && !nonzero; ++k) nonzero = pm[k] != 0.0f;
                    if (nonzero) {
                        std::memcpy(c.raw->snap_pos_mtx, pm, 12 * sizeof(float));
                        std::memcpy(c.raw->snap_nrm_mtx, &g_parse_nrmmtx[nb], 9 * sizeof(float));
                        c.raw->have_snap_mtx = true;
                    }
                }
            }
            if (g_skindbg) {
                g_skin_draws_total++;
                if (vat.posMatIdx) g_skin_draws_pmidx++;
                if (g_state.defPosMtxIdx != 0) g_skin_defnz++;
                if (c.raw->have_snap_mtx) g_skin_snapped++;
            }
            // Normals only matter when a colour channel is lit (XF 0x100e/0x100f
            // bit1); decoding them otherwise is pure cost.
            // Parse-time XF shadow (M1.2): g_state.xf is the renderer's
            // file and lags (or leads) this draw's stream position.
            c.raw->need_normal = ((g_parse_xf[0x00E] >> 1) & 1) ||
                                 ((g_parse_xf[0x00F] >> 1) & 1) ||
                                 ((g_parse_xf[0x010] >> 1) & 1) ||
                                 ((g_parse_xf[0x011] >> 1) & 1) ||
                                 TexGenReadsNormal();
            std::memcpy(c.raw->arrayBase, g_state.arrayBase, sizeof(c.raw->arrayBase));
            std::memcpy(c.raw->arrayStride, g_state.arrayStride, sizeof(c.raw->arrayStride));
            c.raw->count = vtx_count;
            c.raw->bytes.assign(fifo.begin() + offset + 3, fifo.begin() + offset + need);
            static const bool vatdbg = std::getenv("NWII_VATDBG") != nullptr;
            if (vatdbg) {
                // Compact VAT signature: does the computed vsize land the next
                // byte on a plausible opcode? If not, vsize is wrong.
                uint8_t nb = (offset + need < fifo_size) ? fifo[offset + need] : 0;
                bool good = (nb==0||nb==0x08||nb==0x10||nb==0x61||nb==0x40||nb==0x48||
                             (nb>=0x20&&nb<=0x38)||(nb>=0x80&&nb<=0xBF));
                uint32_t sig = (vat.posMatIdx?1:0) | ((uint32_t)vat.posType<<1) |
                    ((uint32_t)vat.posElements<<4) | ((uint32_t)vat.nrmMask<<5) |
                    ((uint32_t)vat.nrmType<<7) | ((uint32_t)vat.nrmElements<<10) |
                    ((uint32_t)vat.clrMask[0]<<11) | ((uint32_t)vat.clrType[0]<<13) |
                    ((uint32_t)vat.texMask[0]<<16) | ((uint32_t)vat.texType[0]<<18);
                static std::map<uint32_t,std::pair<uint64_t,uint64_t>> sigs; // sig->(count,badnext)
                auto& e = sigs[sig]; e.first++;
                if(!good){ e.second++;
                    // scan forward for the next plausible opcode to learn the
                    // TRUE payload size, vs our computed 'need'.
                    size_t scan=offset+3;
                    while(scan<fifo_size && scan<offset+3+(size_t)vtx_count*64){
                        uint8_t b=fifo[scan];
                        if(b==0||b==0x08||b==0x10||b==0x61||b==0x40||b==0x48||
                           (b>=0x20&&b<=0x38)||(b>=0x80&&b<=0xBF)) break;
                        scan++;
                    }
                    size_t actual_payload=scan-(offset+3);
                    static int shown=0;
                    if(shown++<30 && vtx_count>0)
                        printf("[SZERR] vtx_count=%u our_vsize=%zu our_need=%zu actual_payload=%zu actual_per_vtx=%.2f nextbyte=0x%02X\n",
                            vtx_count, VertexSize(vat), need-3, actual_payload,
                            (double)actual_payload/vtx_count, scan<fifo_size?fifo[scan]:0);
                }
                static auto t0=std::chrono::steady_clock::now();
                auto now=std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(now-t0).count()>=2){
                    t0=now;
                    printf("[VAT] sig(posMI,posT,posE,nrmM,nrmT,nrmE,clrM,clrT,texM,texT)=count/badnext vsize:\n");
                    for(auto&kv:sigs){uint32_t g=kv.first;
                        printf("  (%d,%d,%d,%d,%d,%d,%d,%d,%d,%d)=%llu/%llu sz=%zu\n",
                        g&1,(g>>1)&7,(g>>4)&1,(g>>5)&3,(g>>7)&7,(g>>10)&1,(g>>11)&3,(g>>13)&7,(g>>16)&3,(g>>18)&7,
                        (unsigned long long)kv.second.first,(unsigned long long)kv.second.second,VertexSize(vat));}
                    sigs.clear();
                }
            }
            g_prof_draws++;
            g_prof_draw_bytes += need;

            commands.push_back(std::move(c));
            offset += need;
            g_prof_snap_us += std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - sn0).count();
        } else {
            g_prof_unknown++;
            // NWII_DESYNC: histogram the byte the parser can't recognise, plus
            // the 3 bytes before it (where the previous command over/under-ran).
            static const bool desync = std::getenv("NWII_DESYNC") != nullptr;
            if (desync) {
                static uint64_t hist[256] = {0};
                static auto t0 = std::chrono::steady_clock::now();
                hist[fifo[offset]]++;
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(now - t0).count() >= 2) {
                    t0 = now;
                    printf("[DESYNC] top unknown opcodes:");
                    for (int pass = 0; pass < 6; pass++) {
                        int bi = 0; uint64_t bv = 0;
                        for (int i = 0; i < 256; i++) if (hist[i] > bv) { bv = hist[i]; bi = i; }
                        if (!bv) break;
                        printf(" 0x%02X:%llu", bi, (unsigned long long)bv);
                        hist[bi] = 0;
                    }
                    printf("\n");
                    for (int i = 0; i < 256; i++) hist[i] = 0;
                }
            }
            offset++;
        }
    }
}

} 
