#pragma once
// deko3d pipeline/sampler bridge (AI-assisted, T16). Switch-only.
//
// Turns a deko3d-free dkpipe::PipelineDesc / dkpipe::SamplerDesc (PipelineTable.h,
// filled by PipelineTable.inc / T15) into the matching deko3d state structs.
// Pure functions, no GPU calls.
#if defined(__SWITCH__)

#include <deko3d.h>
#include "game/graphics/deko3d_renderer/PipelineTable.h"

namespace dkpipe {

// deko3d state for one pipeline table entry.
struct DkStates {
  DkDepthStencilState ds;
  DkColorState cs;
  DkBlendState bs;
};

// Fill deko3d state for one table entry. Pure function, no GPU calls.
void to_deko3d(const PipelineDesc& p, DkStates* out);

// Fill a DkSampler for one table sampler.
void to_deko3d(const SamplerDesc& s, DkSampler* out);

// Records the complete fixed-function state for one table entry (T35a, AI-assisted).
// Cull mode is always None (GL parity on the tfrag/tie paths). Mirror order:
// depth-stencil → color → blend → blend const → rasterizer → color write → MSAA
// (same as record_splash_3d in dk_device.cpp).
void record_graphics_state(DkCmdBuf cb, const PipelineDesc& p);

// T35b: the draw axes the caller derives from the GOAL draw-mode word
// (derivation spec: docs/deko3d/pipeline_table.md L27 table — T38's job).
// Pure data, dkpipe types only, host-compilable.
struct DrawModeAxes {
  bool depth_test;   // GS Z-test enable
  Compare depth_op;  // when depth_test is false, pass Compare::Always (table rule)
  bool depth_write;
  Blend blend;       // 7-way table blend
  bool clamp_s;      // sampler wrap axes (false = Repeat)
  bool clamp_t;
  bool filt;         // false = Nearest, true = Linear (min and mag, per table)
  bool mipmap;
};

// First kPipelines entry whose 4 state fields equal the axes, else nullptr.
// Entries differing only in alpha-test share one PipelineDesc (alpha-test is
// shader-side), so first-match is exact. sampler_idx (optional out) = the
// kSamplers index whose 5 fields equal the sampler axes; untouched if none.
const PipelineDesc* find_for_draw_mode(const DrawModeAxes& ax,
                                       uint8_t* sampler_idx = nullptr);

}  // namespace dkpipe

#endif  // __SWITCH__
