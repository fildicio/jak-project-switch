#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "common/custom_data/Tfrag3Data.h"
#include "common/util/FileUtil.h"
#include "common/util/Timer.h"
#include "common/versions/versions.h"

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
  Loader(const fs::path& base_path, int max_levels, GameVersion version);
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

  // FIX 76 (AI-assisted): AREA PREFETCH.
  //
  // f74c proved that streaming a level in at the `catchup` budget during live
  // gameplay holds a locked 30 fps - the only remaining problem is that the
  // player *waits* for it (10-15 s of streaming after crossing an area
  // boundary). f75 tried to spend more frame time on the load and was rejected
  // on hardware (10 fps). FIX 76 removes the wait instead: while the game is
  // running normally and the loader is idle, quietly prefetch the most likely
  // NEXT area at the same proven catchup rate, so that by the time the player
  // crosses, the level is already resident and the transition is instant.
  //
  // The next area is predicted from (a) transitions learned at runtime by
  // watching the game's own __pc-set-levels changes, and (b) a static jak1
  // overworld table. A prefetch is always cancellable within one frame: the
  // moment the game asks for a level we are not already fetching, the
  // in-flight prefetch is dropped through the same unload path FIX 63 uses,
  // so a wrong guess can never delay a real load by more than the file read.
  //
  // All of this is invisible to the game: GOAL never reads loader state back,
  // it only pushes __pc-set-levels. A cached level is just... there.
  void record_transitions_locked(const std::vector<std::string>& levels);
  std::optional<std::string> pick_prefetch_target_locked();

  GameVersion m_game_version = GameVersion::Jak1;
  // level currently being fetched/staged as a prefetch ("" = none). Written
  // under m_loader_mutex.
  std::string m_prefetch_target;
  // a cancelled prefetch that still needs cleanup by update() (render thread).
  std::string m_prefetch_discard;
  // previous frame's desired set, for learning transitions.
  std::vector<std::string> m_prev_desired_levels;
  // learned graph: from -> (to -> times observed).
  std::map<std::string, std::map<std::string, int>> m_learned_transitions;
  // levels currently resident ONLY because we prefetched them (caps how much
  // memory prefetching may pin; cleared when the game adopts the level).
  std::unordered_set<std::string> m_prefetch_resident;
  // levels whose prefetch was evicted (or whose file does not exist): do not
  // retry this session, so prefetch can't thrash against eviction.
  std::unordered_set<std::string> m_prefetch_retired;
  // FIX 76d (AI-assisted): a prefetch that got CANCELLED (the player went
  // somewhere else) is too hot to retry immediately - re-staging the same
  // wrong guess right away burns dwell time and costs dispatch hitches for
  // nothing. map level -> earliest allowed retry time.
  std::unordered_map<std::string, std::chrono::steady_clock::time_point>
      m_prefetch_cooldown;
  // FIX 76e (AI-assisted): prefetch miss-backoff state (render thread only).
  // m_last_frame_gap_ms is the raw (clamped) gap of the previous frame - the
  // EMA cannot distinguish "a frame was just missed" from "recovered", since
  // at a healthy locked 30 fps the EMA itself sits at 33.3. The pause
  // counters back a prefetch off for seconds, not frames, after it causes a
  // real miss; see the FIX 76e comment in update().
  double m_last_frame_gap_ms = 0.0;
  int m_prefetch_pause_frames = 0;  // frames of staging pause remaining
  int m_prefetch_pause_next = 30;   // next pause length (doubles per miss, <=240)
  int m_prefetch_clean_streak = 0;  // clean staged frames -> decay pause back to 1s
  bool m_in_update_blocking = false;  // FIX 76e: inside the post-blackout sync sweep
  void purge_retired_levels(TexturePool& tex_pool, bool immediate);
  void flush_texture_garbage();
  // Frees one chunk of reclaimable GPU memory, for GpuBufferPool's out-of-memory
  // retry. Returns false when there is nothing left to give back.
  bool reclaim_gpu_memory(TexturePool& tex_pool);
  void install_buffer_reclaim(TexturePool& tex_pool);
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
  bool m_buffer_reclaim_installed = false;

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
