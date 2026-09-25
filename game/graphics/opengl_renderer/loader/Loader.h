#pragma once

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
  bool upload_textures(Timer& timer, LevelData& data, TexturePool& texture_pool);

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
#endif

  fs::path m_base_path;
  int m_max_levels = 0;

  // FIX 36 Task 3 (AI-assisted): set by the renderer every frame on every
  // platform (see set_blackout); only read by the Switch budget logic.
  bool m_blackout = false;
};
