#pragma once
// deko3d shrub CPU half (AI-assisted, T70/T71). Same shape as TFragmentDk.cpp: walk the jak1
// shrub buckets of a frame's DMA chain, then build a plain draw list (dktf::FrameDraws) that
// a recorder turns into deko3d commands. Plain C++ header: no deko3d includes (repo rule).
#include <cstdint>

#include "game/graphics/deko3d_renderer/TFragmentDk.h"

struct TfragPcPortData;
class Loader;

namespace dkshrub {

// jak1 shrub buckets (buckets.h SHRUB_NORMAL_LEVEL0 = 20, SHRUB_NORMAL_LEVEL1 = 26), GL order.
inline constexpr int kJak1Buckets[2] = {20, 26};
inline constexpr int kNumPasses = 2;

// T70: read both shrub buckets (same reads as GL Shrub::render, jak1: no proto-vis mask).
void capture_chain(const void* data, uint32_t start_offset);
// T70: test access. pass = index into kJak1Buckets. nullptr when the bucket had no shrub data.
const TfragPcPortData* captured_pc(int pass);

// T71: build this frame's draw list from the captured buckets (Loader: Switch only, may be
// nullptr on the host replay).
void build_frame(Loader* loader);
const dktf::FrameDraws& frame_draws();

// T72: record frame_draws() with the SHRUB shaders (dk_tfrag_record.cpp, Switch only).
void record_world(int fb_w, int fb_h);

}  // namespace dkshrub
