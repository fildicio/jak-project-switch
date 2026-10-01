#pragma once

#include "game/graphics/opengl_renderer/loader/common.h"

// ---------------------------------------------------------------------------
// FIX 36 Task 3 (AI-assisted): adaptive per-frame loader budget.
//
// The old constants were flat on Switch (2 ms / 256 KB of texture data per
// frame). At the measured ~13 fps that is ~26 ms of loading work and ~3.3 MB
// of texture upload per second - a city level (tens of MB) took tens of
// seconds to re-enter. Loader::update_frame_budget() retunes these every
// frame: a big budget during blackouts/loading screens (nothing to protect),
// a medium one while the frame rate is healthy, the tight one when we are
// already missing 30 fps. The stages read the globals instead of constants;
// desktop keeps the old fixed values (set once below, never retuned).
// ---------------------------------------------------------------------------
struct LoaderFrameBudget {
  float ms = 4.5f;                  // wall-clock budget per Loader::update()
  u32 tex_bytes = 1024 * 1024;      // TextureLoaderStage byte cap
  u32 stage_kb = 2048;              // per-stage upload byte cap
};
extern LoaderFrameBudget g_loader_budget;

// ---------------------------------------------------------------------------
// FIX 39 "LoadBoost" (AI-assisted): while an area is actually streaming, the
// frame competes with the loader for the same GPU and driver - the texture
// stage measures ~2.2 ms of driver time per texture and a city level has
// hundreds of them. Rendering at a full 1280x720 during that window buys
// nothing the player can see (the world is half-populated anyway) and costs
// the loader time it could be using to finish. So while there is a backlog
// the game renders at 960x540 and the difference goes to the loader; it
// returns to 720p a beat after the backlog clears.
//
// The hysteresis is deliberately asymmetric: enter fast (the hitch is already
// happening), leave slow, so a trickle of small loads can never make the
// resolution flicker - which would be far more objectionable than the lower
// resolution itself.
// ---------------------------------------------------------------------------
void loadboost_set_streaming(bool streaming);
bool loadboost_active();

// ---------------------------------------------------------------------------
// FIX 42 -- DEFERRED MIPMAPS. (AI-assisted)
//
// glGenerateMipmap is ~35% of the cost of getting a texture onto the GPU here:
// `[loader] tex stage: 194 textures, upload 155.1ms, mipgen 82.4ms`. That work
// happens on the render thread, inside the frame, during exactly the window
// where the player is already suffering - a city level uploads hundreds of
// textures and the frame rate collapses while it does.
//
// But mipmaps are not needed for the texture to be *correct*, only for it to
// stop aliasing in the distance. So during a load we upload level 0 and set
// GL_TEXTURE_MAX_LEVEL to 0, which makes the texture mipmap-complete with a
// single level: the renderers can keep binding GL_LINEAR_MIPMAP_LINEAR (they
// set the filter per draw, so we cannot control that from here) and sampling
// just uses level 0. The real mip chain is generated later, a few textures per
// frame, once the loader has no backlog - i.e. paid out of slack instead of
// out of the frame the player is looking at.
//
// Worst case a texture is aliased for a second or so after an area loads.
// ---------------------------------------------------------------------------
// FIX 42a: normally on only for Switch, but it can be forced on for a desktop build
// (-DGOAL_DEFER_MIPMAPS=1) so the deferred path can be validated without a console
// round-trip. This is how the "everything renders black" bug was caught.
#ifndef GOAL_DEFER_MIPMAPS
#ifdef __SWITCH__
#define GOAL_DEFER_MIPMAPS 1
#else
#define GOAL_DEFER_MIPMAPS 0
#endif
#endif

void mipq_defer(u32 gl_texture);
// Generate up to `max_count` deferred mip chains. Returns how many were done.
int mipq_process(int max_count);
size_t mipq_pending();

// ---------------------------------------------------------------------------
// FIX 39 "LoadBoost" (AI-assisted): while an area is actually streaming, the
// frame is competing with the loader for the same GPU/driver - the texture
// stage measures ~2.2 ms of driver time per texture and a city level has
// hundreds of them. Rendering at full 1280x720 during that window buys
// nothing the player can see (the world is half-populated anyway) and costs
// the loader time. So while streaming is in progress the game renders at
// 960x540 and hands the difference to the loader; it goes back to 720p a
// beat after the backlog clears.
//
// Hysteresis is deliberate and asymmetric: enter quickly (the hitch is
// already happening), leave slowly (so a burst of small loads cannot make the
// resolution flicker every few frames, which would be far more objectionable
// than the lower resolution itself).
// ---------------------------------------------------------------------------
void loadboost_set_streaming(bool streaming);
bool loadboost_active();

// ---------------------------------------------------------------------------
// FIX 42 -- DEFERRED MIPMAPS. (AI-assisted)
//
// glGenerateMipmap is ~35% of the cost of getting a texture onto the GPU here:
// `[loader] tex stage: 194 textures, upload 155.1ms, mipgen 82.4ms`. That work
// happens on the render thread, inside the frame, during exactly the window
// where the player is already suffering - a city level uploads hundreds of
// textures and the frame rate collapses while it does.
//
// But mipmaps are not needed for the texture to be *correct*, only for it to
// stop aliasing in the distance. So during a load we upload level 0 and set
// GL_TEXTURE_MAX_LEVEL to 0, which makes the texture mipmap-complete with a
// single level: the renderers can keep binding GL_LINEAR_MIPMAP_LINEAR (they
// set the filter per draw, so we cannot control that from here) and sampling
// just uses level 0. The real mip chain is generated later, a few textures per
// frame, once the loader has no backlog - i.e. paid out of slack instead of
// out of the frame the player is looking at.
//
// Worst case a texture is aliased for a second or so after an area loads.
// ---------------------------------------------------------------------------
void mipq_defer(u32 gl_texture);
// Generate up to `max_count` deferred mip chains. Returns how many were done.
int mipq_process(int max_count);
size_t mipq_pending();

std::vector<std::unique_ptr<LoaderStage>> make_loader_stages();


// FIX 49 (AI-assisted): the rate/did chosen for the most recent mip drain, so the
// "Loader::update slow setup" line can report how a frame's budget was split
// between the texture upload and the mip drain. Diagnostics only.
extern u32 g_last_mip_rate;
extern u32 g_last_mip_did;

// FIX 52 (AI-assisted): how many texture uploads a frame submitted, counted in
// add_texture() so both the Switch atomic path and the desktop banded path are
// covered without touching either. Loader::gpu_cost_probe() consumes and clears
// this: a frame that uploaded nothing has nothing to measure, so it skips the
// fence entirely and normal play pays nothing for this instrumentation.
extern u32 g_loader_gpu_submits_this_frame;
u64 add_texture(TexturePool& pool, const tfrag3::Texture& tex, bool is_common);

// ---------------------------------------------------------------------------
// FIX 48 -- TEXTURE UPLOAD WITHOUT A BYTE SWAP. (AI-assisted)
//
// FIX 47 implemented a byte-swapped texture upload and got the arithmetic wrong,
// rendering every texture purple on hardware. Investigating that showed the swap
// was never required in the first place:
//
//   tfrag3::Texture::data words are 0xAABBGGRR -- see
//   common/texture/texture_conversion.h:219, `(a << 24) | (b << 16) | (g << 8) | r`.
//   GL_UNSIGNED_INT_8_8_8_8_REV names its components MSB->LSB as A,B,G,R, which is
//   that same order -- hence the desktop path works. GL_UNSIGNED_BYTE + GL_RGBA
//   reads four successive bytes as R,G,B,A, which on a little-endian host is the
//   LSB-first reading of that same word. The two formats already describe
//   identical pixels.
//
// So the fix is to upload tex.data unmodified as GL_UNSIGNED_BYTE, which lets the
// implementation do a straight copy instead of a per-pixel format conversion on
// the render thread. prime_texture_swap() and release_texture_swap() are kept as
// no-ops so the loader-thread call site and the Switch/desktop split are
// unchanged; there is no longer anything to stage.
//
// Empirically checked by construction against the _REV definition as the oracle:
// an identity copy matched 4/4 sample words, a 4-byte reversal matched 0/4, a
// rotate-left-8 matched 0/4.
//
// The performance motivation is unchanged and still stands: glTexImage2D(,
// GL_UNSIGNED_INT_8_8_8_8_REV) does a format conversion on the render thread.
// Measured cost in the FIX 46 jak2 log: 0.76 ms/texture at boot rising to
// 2.43 ms/texture later (3.2x degradation), 2132 ms for a single 1222-texture
// level, 7681 ms across one session.
// ---------------------------------------------------------------------------
#ifndef __SWITCH__
// Desktop has no swap cost worth avoiding, so these are deliberate no-ops there:
// the Switch-only signature differences stay out of the shared call sites.
inline void prime_texture_swap(const tfrag3::Texture&) {}
inline void release_texture_swap(const tfrag3::Texture&) {}
inline size_t texture_swap_pending() {
  return 0;
}
#else
void prime_texture_swap(const tfrag3::Texture& tex);
void release_texture_swap(const tfrag3::Texture& tex);
size_t texture_swap_pending();
#endif

class MercLoaderStage : public LoaderStage {
 public:
  MercLoaderStage();
  bool run(Timer& timer, LoaderInput& data) override;
  void reset() override;

 private:
  bool m_done = false;
  bool m_opengl = false;
  bool m_vtx_uploaded = false;
  u32 m_idx = 0;
};