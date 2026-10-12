#pragma once

#include <cstdint>

namespace nwii::runtime {
struct CPUContext;

// Diagnostic interpreter PC trace, off unless NWII_TRACEPC is set, e.g.
// NWII_TRACEPC=2:0xbb0,2:0x36d70?r5,0x80123450 (REL module id : offset in the
// module's first executable section, resolved through the original OSLink
// module list at 0x800030C8; optional ?rN logs only hits with rN != 0, and
// #rN+0xOFF also logs 16 bytes at gpr[N]+OFF; NWII_TRACEPC_MAX caps hits).
// Read-only: it never changes guest registers or memory.
// NWII_SS_HANGDUMP=1|lo-hi: host watchdog that dumps registers, GQRs, OS
// context/thread FPU state and (MTX34RotXYZFIdx) CHR0 data when the CPU PC
// stays in the range ~2 s (ss_boot.cpp hang_dump). Also read-only.
bool ss_pc_trace_enabled();
void ss_pc_trace(CPUContext& ctx, uint32_t pc);
}

// Per-game adapters for Skyward Sword SOUE01, revision 0.
extern "C" {
void SS_SCGetMplsMoviePlay(nwii::runtime::CPUContext& ctx);
void SS_WPADInit(nwii::runtime::CPUContext& ctx);
void SS_KPADInitEx(nwii::runtime::CPUContext& ctx);
void SS_KPADSetConnectCallback(nwii::runtime::CPUContext& ctx);
void SS_WPADRegisterAllocator(nwii::runtime::CPUContext& ctx);
void SS_WPADProbe(nwii::runtime::CPUContext& ctx);
void SS_KPADReadEx(nwii::runtime::CPUContext& ctx);
void SS_KPADSetMplsWorkarea(nwii::runtime::CPUContext& ctx);
void SS_WPADiGetMplsStatus(nwii::runtime::CPUContext& ctx);
void SS_WPADiControlMpls(nwii::runtime::CPUContext& ctx);
void SS_KPADGetUnifiedWpadStatus(nwii::runtime::CPUContext& ctx);
void SS_WPADGetInfoAsync(nwii::runtime::CPUContext& ctx);
void SS_WPADCanSendStreamData(nwii::runtime::CPUContext& ctx);
void SS_WPADGetRadioSensitivity(nwii::runtime::CPUContext& ctx);
[[noreturn]] void SS_DvdLoadFailure(nwii::runtime::CPUContext& ctx);
[[noreturn]] void SS_ExceptionWaitFailure(nwii::runtime::CPUContext& ctx);
[[noreturn]] void SS_sAssertTrace(nwii::runtime::CPUContext& ctx);
[[noreturn]] void SS_EggAssertTrace(nwii::runtime::CPUContext& ctx);
}
