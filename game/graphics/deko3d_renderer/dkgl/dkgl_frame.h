#pragma once
// T107 (AI-assisted): dkgl frame hook -- the UNCHANGED GL ocean renderers
// (opengl_renderer/ocean/*.cpp) run inside the dk frame through the dkgl layer.
//
// Two halves, because the dk frame acks the DMA chain right after its captures (the EE
// rewrites that memory afterwards, dk_renderer.cpp T60) while the GPU commands are recorded
// later, inside the world recorder:
//   capture_frame()  -- at capture time, chain still valid: runs the GL renderers of
//                       buckets 4 (ocean-mid-far) and 63 (ocean-near); the Switch emitter
//                       records their GPU work into dkgl's OWN command lists (own memory,
//                       3 fence-protected slots).
//   record_bucket()  -- in the world recorder, at the bucket's place in the GL bucket order:
//                       calls those lists from the frame command buffer.
// Fallbacks: gk_dkocean_native.txt keeps the native dkocean path; any init failure logs
// "[dkgl] ..." and leaves active() false (native ocean). No runtime threads.

#include "common/common_types.h"

class TexturePool;
class Loader;

namespace dkgl {

// Once, on the render thread, after the dk device, the dktexgpu store, the census glad
// stubs, the dk TexturePool and the dk Loader are up. Jak 1 only.
bool init(TexturePool* pool, Loader* loader);
bool active();
// #19 (AI-assisted): the GL Merc2 draws the characters through dkgl (default); the native
// MercDk must then skip its draws.
bool merc_active();
// #19: true when the GL EyeRenderer runs in bucket 54 (replaces the native eye pass)
bool eyes_active();
// Capture-time half (dk_renderer capture block, dma_mutex held, chain not yet acked).
void capture_frame(const void* chain_data, u32 chain_offset);
// #19 (AI-assisted): VRAM lookups for the eye bucket (54) during the next capture_frame;
// a TexturePool::VramTable* taken at bucket 54's upload position, or nullptr = live VRAM.
void set_eye_vram_snapshot(const void* table);
// World-recorder half: 4 = ocean-mid-far, 63 = ocean-near. Returns false if nothing was
// captured for that bucket (caller keeps going; the native ocean is not involved).
bool record_bucket(int bucket);
// pmode_alp fade (GL do_pcrtc_effects): set from the EE thread, captured with the frame,
// recorded after the last world pass so it covers the HUD too.
void set_pmode_alp(float alp);
bool record_blackout();
// The frame target size, for the GL default viewport (OpenGLRenderer sets it per frame).
void set_frame_size(int w, int h);

}  // namespace dkgl
