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

std::vector<std::unique_ptr<LoaderStage>> make_loader_stages();
u64 add_texture(TexturePool& pool, const tfrag3::Texture& tex, bool is_common);

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