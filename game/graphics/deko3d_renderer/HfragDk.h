#pragma once
// T113 (AI-assisted): deko3d Hfrag pass (jak1 bucket 8) -- the distant mountain
// terrain. Port of GL background/Hfrag.cpp; the vertex/index buffers are already
// uploaded by the shared HfragLoaderStage (LoaderStages.cpp), this is the render
// half only. Plain C++ header (no deko3d includes), like TFragmentDk.h.
#include <cstdint>

class Loader;

namespace dkhfrag {

// Render thread, BEFORE the chain is acknowledged: read the HFRAG bucket's
// TfragPcPortData packets (level name + camera). No GPU access.
void capture_chain(const void* data, uint32_t start_offset);

// Render thread, after Loader::update and dktf::build_frame (fog comes from
// dktf::frame_draws()): resolve levels, cull corners, update tod palettes.
void build_frame(Loader* loader);

// Inside present_frame's world recorder, after dktf::record_world (GL bucket
// order: tfrag 6/7 -> hfrag 8 -> tie 9).
void record_world(int fb_w, int fb_h);

}  // namespace dkhfrag
