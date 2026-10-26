#pragma once
// T113 (AI-assisted): deko3d Hfrag pass (jak1 bucket 8) -- the distant mountain
// terrain. Port of GL background/Hfrag.cpp; the vertex/index buffers are already
// uploaded by the shared HfragLoaderStage (LoaderStages.cpp), this is the render
// half only. Plain C++ header (no deko3d includes), like TFragmentDk.h.
#include <cstdint>

struct TfragPcPortData;  // background_common.h (pointer-only use here, like TieDk.h)

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

// FIX 117 (AI-assisted): capture/build diagnostics. T113's first hardware runs produced
// no "[dk] hfrag level" line at all -- every early-out was silent, so they are counted
// here, summarised by the periodic "[dk] hfrag cap" line, and read by the host replay
// test. Plain struct on purpose: no deko3d types in this header.
struct Diag {
  uint64_t calls = 0;       // capture_chain calls that got chain data
  uint64_t head_call = 0;   // bucket 8 head was a CALL (renderer didn't run)
  uint64_t empty = 0;       // bucket ran but held no packets
  uint64_t walk_break = 0;  // packet loop stopped on an unexpected tag
  uint64_t packets = 0;     // TfragPcPortData packets captured
  uint64_t no_level = 0;    // level lookup miss (the name is logged once)
  uint64_t bad_data = 0, no_buffers = 0, load_fail = 0;  // load_level failures
  uint64_t level_ok = 0;    // levels resolved (loaded or reused) this run
  uint32_t vis_corners = 0, used_buckets = 0;  // last build (host replay asserts on these)
};

const Diag& diag();

// FIX 117 (AI-assisted): tests only -- inject one PC_PORT packet (level name +
// camera) as if the running bucket had sent it, so the host replay can drive
// build_frame against any level. The recorded chain's bucket 8 is EMPTY (see
// test_dk_hfrag_replay.cpp), so the load/cull path needs synthetic packets.
void capture_packet(const TfragPcPortData* pc);

}  // namespace dkhfrag
