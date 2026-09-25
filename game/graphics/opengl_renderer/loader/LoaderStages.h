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