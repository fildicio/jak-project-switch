#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "common/custom_data/Tfrag3Data.h"
#include "common/util/FileUtil.h"
#include "common/util/Timer.h"

#include "game/graphics/opengl_renderer/loader/common.h"
#include "game/graphics/texture/TexturePool.h"

class Loader {
 public:
  static constexpr float TIE_LOAD_BUDGET = 1.5f;
#ifdef __SWITCH__
  // Tighter per-frame budget on Switch - the loader shares the frame with the
  // game (see FIX 9 in SWITCH_PORT_SESSION_NOTES.md).
  static constexpr float SHARED_TEXTURE_LOAD_BUDGET = 1.5f;
#else
  static constexpr float SHARED_TEXTURE_LOAD_BUDGET = 3.f;
#endif
  Loader(const fs::path& base_path, int max_levels);
  ~Loader();
  void update(TexturePool& tex_pool);
  void update_blocking(TexturePool& tex_pool);
  // FIX 36 Task 3 (AI-assisted): the renderer tells us every frame whether the
  // screen is currently black (loading screen); during a blackout there is no
  // frame rate to protect and the loader budget is raised a lot.
  void set_blackout(bool blackout) { m_blackout = blackout; }
  const LevelData* get_tfrag3_level(const std::string& level_name);
  std::optional<MercRef> get_merc_model(const char* model_name);
  const tfrag3::Level& load_common(TexturePool& tex_pool, const std::string& name);
  void set_want_levels(const std::vector<std::string>& levels);
  void set_active_levels(const std::vector<std::string>& levels);
  std::vector<LevelData*> get_in_use_levels();
  void draw_debug_window();
  void debug_print_loaded_levels();
  void request_reload_all() { m_want_reload = true; }
  void request_reload_level(const std::string& name) { m_single_level_to_reload = name; }
  void request_reload_common() { m_want_reload_common = true; }

 private:
  void loader_thread();

  const std::string* pick_eviction_victim();
  void unload_level_gpu_objects(LevelData& lev, TexturePool& tex_pool);
  void purge_retired_levels(TexturePool& tex_pool, bool immediate);
  void flush_texture_garbage();
  void do_reload(TexturePool& tex_pool);
  void do_reload_common(TexturePool& tex_pool);
  void do_reload_level(const std::string& name, TexturePool& tex_pool);
#ifdef __SWITCH__
  // FIX 36 Task 3 (AI-assisted): adaptive per-frame loader budget (see
  // LoaderStages.h). Render thread only, called at the top of update().
  void update_frame_budget();
  // FIX 36 Task 3: recycle levels only when memory actually demands it.
  bool loader_under_pressure();
  // FIX 52 (AI-assisted): ask the GPU how long the loader's own submitted work
  // actually took. Render thread only; called at the END of update().
  // See the definition for why the wall-clock loader_timer cannot answer this.
  double gpu_cost_probe();

  // ---------------------------------------------------------------------------
  // FIX 58 (AI-assisted): RETIRED-LEVEL WARM CACHE.
  //
  // Hardware telemetry (FIX 57 run) showed the real cost of a city section
  // transition is not disk and not the per-frame upload budget: the game drops
  // shared levels (lwidea/ctywide) on exit, the loader evicts them, and the
  // next section pays a FULL GPU re-upload - lwidea is 1222 textures, ~12
  // seconds at the paced upload rate, every single transition. The .fr3 file
  // load is sub-second; the uploads are everything.
  //
  // So instead of destroying an evicted level's GPU objects, it is moved into
  // a small warm cache (textures + buffers kept valid, texture-pool
  // registrations intact). When the game wants the level back, set_want_levels
  // revives it with zero re-uploads. The cache is bounded by texture bytes and
  // level count; LRU overflow performs the real unloads (FIX 58b: a separate
  // buffer-pool-pressure relief valve proved self-defeating on hardware -
  // pool-free dips below 16MB are normal during streaming, so it drained the
  // cache every frame).
  void retire_to_cache(const std::string& name, std::unique_ptr<LevelData> lev,
                       TexturePool& tex_pool);
  // Actually destroy the oldest / a named retired level (full GPU teardown).
  // Both return false if nothing was dropped.
  bool drop_oldest_retired(TexturePool& tex_pool);
  bool drop_retired_level(const std::string& name, TexturePool& tex_pool);
  // Move a cached level back to live. Caller must hold m_loader_mutex.
  // Render-thread GL safety: no GL calls happen here, only pointer moves.
  bool revive_from_cache(const std::string& name);
#endif

  // used by game and loader thread
  std::unordered_map<std::string, std::unique_ptr<LevelData>> m_initializing_tfrag3_levels;

  LevelData m_common_level;

  std::string m_level_to_load;

  std::thread m_loader_thread;
  std::mutex m_loader_mutex;
  std::condition_variable m_loader_cv;
  std::condition_variable m_file_load_done_cv;
  bool m_want_shutdown = false;
  std::atomic<bool> m_want_reload{false};
  std::atomic<bool> m_want_reload_common{false};
  std::string m_single_level_to_reload;
  std::string m_selected_level_for_reload;
  uint64_t m_id = 0;

  // used only by game thread
  std::unordered_map<std::string, std::unique_ptr<LevelData>> m_loaded_tfrag3_levels;

  std::unordered_map<std::string, std::vector<MercRef>> m_all_merc_models;

  std::vector<std::string> m_desired_levels;
  std::vector<std::string> m_active_levels;
  std::vector<std::unique_ptr<LoaderStage>> m_loader_stages;
  std::vector<GLuint> m_garbage_textures;
  std::vector<GLuint> m_garbage_buffers;

  // FIX 33 (AI-assisted): pooled loader GL buffers (see GpuBufferPool.h).
  // Render thread only.
  GpuBufferPool m_buffer_pool;

#ifdef __SWITCH__
  // FIX 33: telemetry frame counter for the periodic [loader] status line.
  int m_stats_frame_count = 0;
  // FIX 36 Task 3 (AI-assisted): adaptive-budget state (Switch only; see
  // update_frame_budget). The frame-gap EMA (~8-frame average) decides how
  // much streaming work the loader may do while gameplay is on screen.
  double m_frame_gap_ema_ms = 0.0;
  std::chrono::steady_clock::time_point m_last_update_tp{};
  const char* m_budget_mode = "";
  // FIX 36 Task 3: request -> ready timing, for "[loader] level X ready in".
  std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_load_start;
  // FIX 52 (AI-assisted): the loader's *measured GPU* cost per frame, in ms.
  //
  // m_frame_gap_ema_ms above times the whole frame (renderer included), so it says
  // nothing about how much of that the loader caused. loader_timer in update() has
  // the same blind spot from the other side: it times the calls that *submit* GL
  // work, and glTexSubImage2D / glGenerateMipmap return as soon as the command is
  // queued. The GPU cost then surfaces later, inside the swapchain acquire that the
  // [phase] line attributes to `pcrtc`. That is why the logs can show
  // `[phase] loader 0.01 | pcrtc 22.33` and `slow setup: 20.1ms` in the same frame
  // without contradicting each other -- neither number is the loader's real cost.
  //
  // This EMA is that cost, obtained by fencing after the loader's submission. It is
  // what update_frame_budget() clamps the texture budget against.
  double m_loader_gpu_ema_ms = 0.0;
  // Nonzero while a fence from a previous frame is still outstanding. Only one is
  // ever in flight: the probe reuses it, so the probe measures consecutive frames
  // rather than allocating a fence per frame.
  void* m_gpu_fence = nullptr;
  // FIX 52: the last raw probe result, for the periodic [loader] telemetry line.
  // Negative means "at least this much" -- the probe hit its timeout.
  double m_loader_gpu_last_ms = 0.0;

  // FIX 58 (AI-assisted): the warm cache itself (see retire_to_cache above).
  // A level name is in exactly one of m_loaded_tfrag3_levels /
  // m_initializing_tfrag3_levels / m_retired_levels at any time. Guarded by
  // m_loader_mutex (revive runs on the game thread).
  std::unordered_map<std::string, std::unique_ptr<LevelData>> m_retired_levels;
  std::vector<std::string> m_retired_lru;  // front = oldest
  size_t m_retired_tex_bytes = 0;
  // The count of submissions for the current frame lives in LoaderStages as
  // g_loader_gpu_submits_this_frame (defined in LoaderStages.cpp), because that is
  // where uploads actually happen; see the header comment on it.
#endif

  fs::path m_base_path;
  int m_max_levels = 0;

  // FIX 46 (AI-assisted): how many levels the loader may keep resident. Derived
  // from m_max_levels (the game's LEVEL_TOTAL) so it can never be smaller than
  // what the game legitimately holds -- the old hardcoded 8 was smaller than
  // jak3's 11, which forced evict/re-request/re-upload churn every frame. See
  // pick_eviction_victim().
  int max_live_levels() const {
    constexpr int kFloor = 8;
    return std::max(kFloor, m_max_levels + 2);
  }

  // FIX 36 Task 3 (AI-assisted): set by the renderer every frame on every
  // platform (see set_blackout); only read by the Switch budget logic.
  bool m_blackout = false;
};
