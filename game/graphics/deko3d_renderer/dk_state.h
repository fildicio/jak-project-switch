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

}  // namespace dkpipe

#endif  // __SWITCH__
