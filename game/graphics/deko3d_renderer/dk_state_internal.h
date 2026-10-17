#pragma once

/*
 * @file dk_state_internal.h
 * deko3d migration P3.0 (T26, AI-assisted): FIX64_SUB blend const at bind time.
 *
 * Closes the gap documented in dk_state.cpp (to_blend, the Fix64Sub case):
 * DkBlendState has no blend-color field, so the GL glBlendColor(0.5,...) equivalent
 * must be emitted on the command buffer when a Fix64Sub pipeline is bound, via
 * dkCmdBufSetBlendConst (deko3d.h:1293). Following the T22a precedent (the
 * deko3d-internal *_internal.h that may include <deko3d.h> because it is only ever
 * included from #if defined(__SWITCH__) code), the declaration lives here instead
 * of in dk_state.h so the host-testable pipeline table / deko3d-free headers stay
 * clean. INERT: no bind path calls this until P3.2.
 */

#if defined(__SWITCH__)

#include <deko3d.h>                // DkCmdBuf + dkCmdBufSetBlendConst (deko3d.h:1293)
#include "game/graphics/deko3d_renderer/dk_state.h"  // dkpipe::Blend

namespace dkpipe {

//! Emit the FIX64_SUB blend constant (glBlendColor(0.5,0.5,0.5,0.5)) onto the
//! command buffer when a Fix64Sub pipeline is bound. All other Blend values are
//! a no-op (their blend equation needs no constant). Exhaustive, no default: a
//! future Blend value fails to compile here.
void apply_blend_const(DkCmdBuf cb, Blend b);

}  // namespace dkpipe

#endif  // __SWITCH__
