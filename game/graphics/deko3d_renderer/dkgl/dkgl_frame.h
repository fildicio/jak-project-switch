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
class TextureAnimator;
class DmaFollower;
class Loader;
namespace tfrag3 {
struct Level;
}

namespace dkgl {

// jak2 (AI-assisted): the GAME.fr3 common level from Loader::load_common. Call before init;
// its index textures fill the Merc2 anim slots (Jak's face/hair/eyebrow) with static images.
void set_common_level(const tfrag3::Level* level);

// Once, on the render thread, after the dk device, the dktexgpu store, the census glad
// stubs, the dk TexturePool and the dk Loader are up. Jak 1; jak 2 since §7.2 phase 1
// (ocean + Merc2 + Generic2 + sky-draw/screen-filter, shadow2/warp still inert).
bool init(TexturePool* pool, Loader* loader);
// The active dkgl bucket table, in GL dispatch order (jak1 or jak2 ids). For the world
// recorder: record_bucket(bucket_at(i)) in order replays the whole captured frame.
int bucket_count();
int bucket_at(int i);
bool active();
// #19 (AI-assisted): the GL Merc2 draws the characters through dkgl (default); the native
// MercDk must then skip its draws.
bool merc_active();
// #19: true when the GL EyeRenderer runs in bucket 54 (replaces the native eye pass)
bool eyes_active();
// Capture-time half (dk_renderer capture block, dma_mutex held, chain not yet acked).
void capture_frame(const void* chain_data, u32 chain_offset);
// #16/#19 (AI-assisted): capture_frame in steps, so the dkgl buckets run inside the texture
// upload replay at their GL position. begin() returns false when nothing will be captured;
// until(b) runs the dkgl buckets below b; end() runs the rest and closes the frame.
bool capture_begin(const void* chain_data, u32 chain_offset);
void capture_until(int bucket);
void capture_end();
// counts frames captured through begin/until/end (log only)
void note_interleaved_frame();
// World-recorder half: 4 = ocean-mid-far, 63 = ocean-near. Returns false if nothing was
// captured for that bucket (caller keeps going; the native ocean is not involved).
bool record_bucket(int bucket);
// pmode_alp fade (GL do_pcrtc_effects): set from the EE thread, captured with the frame,
// recorded after the last world pass so it covers the HUD too.
void set_pmode_alp(float alp);
bool record_blackout();
// T125 (AI-assisted): the UNCHANGED GL TextureAnimator (jak2 skull gems etc.) through dkgl.
// Dispatched from the SpriteDk tex-bucket walk at PC_PORT imm-12, inside
// capture_begin/capture_end; begin/end bracket one handle_texture_anim_data call (its GL
// draws record into the kTexAnimList segments). record_tex_anim() calls them in the world
// recorder right before the native sprite bucket draws.
TextureAnimator* texture_animator();
bool tex_anim_begin();
void tex_anim_end();
bool record_tex_anim();
// T126 (AI-assisted): the UNCHANGED GL GlowRenderer (jak2 light glows: lamps, candles, sky
// flares) through dkgl, occlusion probe included. glow_dma walks the glow section of the
// sprite bucket (GL Sprite3::glow_dma_and_draw) between glow_begin/glow_end; record_glow()
// calls the recorded lists in the world recorder right after the native sprite bucket (GL
// draws the glow after the HUD sprites). A/B flag: gk_dkgl_noglow.txt.
bool glow_begin();
void glow_dma(DmaFollower& dma);
void glow_end();
bool record_glow();
// The frame target size, for the GL default viewport (OpenGLRenderer sets it per frame).
void set_frame_size(int w, int h);

}  // namespace dkgl
