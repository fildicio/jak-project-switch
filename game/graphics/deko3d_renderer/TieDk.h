#pragma once
// deko3d tie CPU half (AI-assisted, T73/T74). Same shape as ShrubDk: walk the jak1 tie
// buckets of a frame's DMA chain, then build a dktf::FrameDraws list for the recorder.
// Only TieCategory::NORMAL draws (no envmap, no wind yet). Plain C++ header.
#include <cstdint>

#include "game/graphics/deko3d_renderer/TFragmentDk.h"

struct TfragPcPortData;
class Loader;

namespace dktie {

// jak1 tie buckets (buckets.h TIE_LEVEL0 = 9, TIE_LEVEL1 = 16), GL order.
inline constexpr int kJak1Buckets[2] = {9, 16};
inline constexpr int kNumPasses = 2;

// T73: read both tie buckets (same reads as GL Tie3::set_up_common_data_from_dma, jak1).
void capture_chain(const void* data, uint32_t start_offset);
// T73: test access. nullptr when the bucket had no tie data.
const TfragPcPortData* captured_pc(int pass);
// T73: envmap color of that pass (raw 4 floats from the DMA, before GL's /128*2 scaling).
const float* captured_envmap(int pass);

// T74: build this frame's draw list (frustum-culled like GL, NORMAL category only).
void build_frame(Loader* loader);
const dktf::FrameDraws& frame_draws();

// T76: record frame_draws() with the TFRAG3 shaders (dk_tfrag_record.cpp, Switch only).
void record_world(int fb_w, int fb_h);

}  // namespace dktie
