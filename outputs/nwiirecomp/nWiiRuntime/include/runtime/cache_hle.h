#pragma once
// HLE replacements for the SDK cache-maintenance and sync routines (M2.2).
// They are bound per game through [hle_hooks] in the recompiler TOML, e.g.
//   "803A4590" = "HLE_DCFlushRange"
// The generator then emits `HLE_DCFlushRange(ctx); ctx.pc = ctx.lr;` as the
// body of the recompiled function, so recompiled callers and RELs running in
// the interpreter (which call into the DOL through the same functions) both
// reach the handler.
#include "runtime/cpu_context.h"

extern "C" {
void HLE_DCFlushRange(nwii::runtime::CPUContext &ctx);
void HLE_DCStoreRange(nwii::runtime::CPUContext &ctx);
void HLE_DCFlushRangeNoSync(nwii::runtime::CPUContext &ctx);
void HLE_DCStoreRangeNoSync(nwii::runtime::CPUContext &ctx);
void HLE_DCInvalidateRange(nwii::runtime::CPUContext &ctx);
void HLE_DCZeroRange(nwii::runtime::CPUContext &ctx);
void HLE_ICInvalidateRange(nwii::runtime::CPUContext &ctx);
void HLE_PPCSync(nwii::runtime::CPUContext &ctx);
}
