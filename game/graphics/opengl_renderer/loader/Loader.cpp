#include "Loader.h"

#include <ranges>
#include <cstring>

#include "common/global_profiler/GlobalProfiler.h"
#include "common/util/FileUtil.h"
#include "common/util/Timer.h"
#include "common/util/compress.h"

#include "game/graphics/opengl_renderer/loader/LoaderStages.h"

#if defined(__SWITCH__)
#include "game/switch/imgui_stub.h"
#else
#include "third-party/imgui/imgui.h"
#endif

Loader::Loader(const fs::path& base_path, int max_levels)
    : m_base_path(base_path), m_max_levels(max_levels) {
  m_loader_thread = std::thread(&Loader::loader_thread, this);
  m_loader_stages = make_loader_stages();
}

Loader::~Loader() {
  {
    std::lock_guard<std::mutex> lk(m_loader_mutex);
    m_want_shutdown = true;
    m_loader_cv.notify_all();
  }
  m_loader_thread.join();
}

/*!
 * Try to get a loaded level by name. It may fail and return nullptr.
 * Getting a level will reset the counter for the level and prevent it from being kicked out
 * for a little while.
 *
 * This is safe to call from the graphics thread
 */
const LevelData* Loader::get_tfrag3_level(const std::string& level_name) {
  std::unique_lock<std::mutex> lk(m_loader_mutex);
  const auto& existing = m_loaded_tfrag3_levels.find(level_name);
  if (existing == m_loaded_tfrag3_levels.end()) {
    return nullptr;
  } else {
    existing->second->frames_since_last_used = 0;
    return existing->second.get();
  }
}

void Loader::debug_print_loaded_levels() {
  std::unique_lock<std::mutex> lk(m_loader_mutex);
  for (const auto& [name, _] : m_loaded_tfrag3_levels) {
    fmt::print("{}\n", name);
  }
}

/*!
 * The game calls this to give the loader a hint on which levels we want.
 * If the loader is not busy, it will begin loading the level.
 * This should be called on every frame.
 */
void Loader::set_want_levels(const std::vector<std::string>& levels) {
  std::unique_lock<std::mutex> lk(m_loader_mutex);
  m_desired_levels = levels;
  if (!m_level_to_load.empty()) {
    // can't do anything, we're loading a level right now
    return;
  }

  if (!m_initializing_tfrag3_levels.empty()) {
    // can't do anything, we're initializing a level right now
    return;
  }

  // loader isn't busy, try to load one of the requested levels.
  for (auto& lev : levels) {
    auto it = m_loaded_tfrag3_levels.find(lev);
    if (it == m_loaded_tfrag3_levels.end()) {
      // we haven't loaded it yet. Request this level to load and wake up the thread.
      m_level_to_load = lev;
#ifdef __SWITCH__
      // FIX 36 Task 3 (AI-assisted): start the "ready in" clock at request
      // time - it is read and logged when the level finishes staging, inside
      // update()'s finish-stages block, under this same mutex.
      m_load_start[lev] = std::chrono::steady_clock::now();
#endif
      lk.unlock();
      m_loader_cv.notify_all();
      return;
    }
  }
}

#ifdef __SWITCH__
/*!
 * FIX 36 Task 3 (AI-assisted): adaptive loader budget.
 *
 * The flat "2 ms / 256 KB per frame" Switch budget from FIX 33 was tuned for
 * steady gameplay, but it also applied during blackout loads where there is
 * nothing to protect - the game is already stalled behind update_blocking().
 * A city re-entry then paid tens of seconds of 256 KB/frame uploads
 * (SWITCH_FIX36_AGENT_BRIEF.md §1C).
 *
 * Now the budget follows what the frame can actually afford, decided from a
 * frame-gap EMA (~8-frame average, single gaps clamped at 200 ms so one hitch
 * can't pin it high):
 *   blackout - the game is stalled waiting for us: go big (12 ms / 4 MB).
 *   healthy  - EMA under 25 ms: stream faster (4 ms / 1 MB).
 *   lean     - around 30 fps: the old flat budget (2 ms / 256 KB).
 *   struggle - badly missing 30 fps: don't make it worse (1 ms / 128 KB).
 * Render thread only, called once per update() before the stages run.
 */
void Loader::update_frame_budget() {
  const auto now = std::chrono::steady_clock::now();
  if (m_last_update_tp.time_since_epoch().count() > 0) {
    double gap = std::chrono::duration<double, std::milli>(now - m_last_update_tp).count();
    gap = std::min(gap, 200.0);
    m_frame_gap_ema_ms += (gap - m_frame_gap_ema_ms) * 0.125;
  }
  m_last_update_tp = now;

  // FIX 38 (AI-assisted): BACKLOG BEATS FRAME TIME.
  //
  // Driving the budget purely from the frame gap created a death spiral: the
  // city runs at ~13 fps, so the EMA sat at 35-85 ms permanently, so the
  // loader sat in "struggle" at 1 ms / 128 KB per frame = ~1.6 MB/s. That is
  // why the city took forever to repopulate and why NPCs and the zoomer were
  // missing for tens of seconds after re-entry (hardware log, 2026-09-25:
  // "budget ms=1.0 tex_kb=128 mode=struggle" alternating with "lean", with
  // live=7 want=5 the whole time). The logic was exactly backwards - it
  // throttled hardest precisely when there was most to load.
  //
  // Now: if there is anything queued, we are in catch-up and get a real
  // budget. Frame time may only modulate WITHIN catch-up, never below the
  // floor. Dropping a few frames while the world populates is what the player
  // wants; a 30-second wait is not.
  size_t pending = 0;
  {
    std::unique_lock<std::mutex> lk(m_loader_mutex);
    pending = m_initializing_tfrag3_levels.size() + (m_level_to_load.empty() ? 0 : 1);
    if (m_desired_levels.size() > m_loaded_tfrag3_levels.size()) {
      pending += m_desired_levels.size() - m_loaded_tfrag3_levels.size();
    }
  }

  LoaderFrameBudget want;
  const char* mode;
  if (m_blackout) {
    want = {12.f, 4 * 1024 * 1024, 4096};
    mode = "blackout";
  } else if (pending > 0) {
    // catch-up: floor of 4 ms / 1 MB, more when the frame can afford it
    if (m_frame_gap_ema_ms > 45.0) {
      want = {4.f, 1024 * 1024, 1024};
      mode = "catchup-floor";
    } else {
      want = {8.f, 2 * 1024 * 1024, 2048};
      mode = "catchup";
    }
  } else if (m_frame_gap_ema_ms > 45.0) {
    want = {1.f, 128 * 1024, 256};
    mode = "idle-struggle";
  } else if (m_frame_gap_ema_ms > 25.0) {
    want = {2.f, 256 * 1024, 512};
    mode = "idle-lean";
  } else {
    want = {4.f, 1024 * 1024, 1024};
    mode = "idle-healthy";
  }
  if (std::strcmp(mode, m_budget_mode) != 0) {
    m_budget_mode = mode;
    g_loader_budget = want;
    fmt::print("[loader] budget ms={:.1f} tex_kb={} mode={} (ema {:.1f}ms, pending {})\n",
               (double)g_loader_budget.ms, g_loader_budget.tex_bytes / 1024, mode,
               m_frame_gap_ema_ms, pending);
  }
}

/*!
 * FIX 36 Task 3 (AI-assisted): real GPU-buffer-pool pressure. FIX 33 evicted
 * levels on a frame counter while the pool sat on 208 MB of free buffers.
 * The live-level hard cap itself lives in pick_eviction_victim(); this is
 * the "is the recycling pool actually running dry" signal. Render thread only.
 */
bool Loader::loader_under_pressure() {
  // pooled_bytes() is free recycled-buffer bytes (see the [loader] telemetry);
  // below ~16 MB a fresh stage upload would have to extend the arena.
  constexpr size_t kPressureFreeBytes = 16 * 1024 * 1024;
  return m_buffer_pool.pooled_bytes() < kPressureFreeBytes;
}
#endif

/*!
 * The game calls this to tell the loader that we absolutely want these levels active.
 * This will NOT trigger a load!
 */
void Loader::set_active_levels(const std::vector<std::string>& levels) {
  std::unique_lock<std::mutex> lk(m_loader_mutex);
  m_active_levels = levels;
}

/*!
 * Get all levels that are in memory and used very recently.
 */
std::vector<LevelData*> Loader::get_in_use_levels() {
  std::vector<LevelData*> result;
  std::unique_lock<std::mutex> lk(m_loader_mutex);

  for (auto& [name, lev] : m_loaded_tfrag3_levels) {
    if (lev->frames_since_last_used < 5) {
      result.push_back(lev.get());
    }
  }
  return result;
}

void Loader::draw_debug_window() {
  ImGui::Begin("Loader");

  if (!m_selected_level_for_reload.empty() &&
      m_loaded_tfrag3_levels.find(m_selected_level_for_reload) == m_loaded_tfrag3_levels.end()) {
    m_selected_level_for_reload.clear();
  }

  const char* preview = m_selected_level_for_reload.empty() ? "(select a level)"
                                                            : m_selected_level_for_reload.c_str();
  if (ImGui::BeginCombo("Level Select", preview)) {
    for (const auto& [name, data] : m_loaded_tfrag3_levels) {
      bool selected = (name == m_selected_level_for_reload);
      if (ImGui::Selectable(name.c_str(), selected)) {
        m_selected_level_for_reload = name;
      }
      if (selected) {
        ImGui::SetItemDefaultFocus();
      }
    }
    ImGui::EndCombo();
  }

  bool single_reload_pending = !m_single_level_to_reload.empty();
  bool can_reload_selected =
      !m_selected_level_for_reload.empty() && !single_reload_pending && !m_want_reload;
  if (!can_reload_selected) {
    ImGui::BeginDisabled();
  }
  if (ImGui::Button("Force Reload Selected")) {
    m_single_level_to_reload = m_selected_level_for_reload;
  }
  if (!can_reload_selected) {
    ImGui::EndDisabled();
  }
  ImGui::SameLine();
  if (ImGui::Button("Force Reload Common")) {
    m_want_reload_common = true;
  }
  ImGui::SameLine();
  if (ImGui::Button("Force Reload All")) {
    m_want_reload = true;
  }
  ImGui::SameLine();
  if (m_want_reload) {
    ImGui::TextUnformatted("(waiting for full reload...)");
  } else if (single_reload_pending) {
    ImGui::Text("(waiting to reload %s...)", m_single_level_to_reload.c_str());
  }
  ImGui::Separator();

  std::unique_lock<std::mutex> lk(m_loader_mutex);
  ImVec4 blue(0.3, 0.3, 0.8, 1.0);
  ImVec4 red(0.8, 0.3, 0.3, 1.0);
  ImVec4 green(0.3, 0.8, 0.3, 1.0);

  if (!m_desired_levels.empty()) {
    ImGui::Text("desired levels");
    for (auto& lev : m_desired_levels) {
      auto lev_color = red;
      if (m_initializing_tfrag3_levels.find(lev) != m_initializing_tfrag3_levels.end()) {
        lev_color = blue;
      }
      if (m_loaded_tfrag3_levels.find(lev) != m_loaded_tfrag3_levels.end()) {
        lev_color = green;
      }
      ImGui::TextColored(lev_color, "%s", lev.c_str());
      ImGui::SameLine();
    }
    ImGui::NewLine();
    ImGui::Separator();
  }

  if (!m_initializing_tfrag3_levels.empty()) {
    ImGui::Text("init levels");
    for (auto& lev : m_initializing_tfrag3_levels) {
      ImGui::TextColored(blue, "%s", lev.first.c_str());
      ImGui::SameLine();
    }
    ImGui::NewLine();
    ImGui::Separator();
  }

  if (!m_loaded_tfrag3_levels.empty()) {
    ImGui::Text("loaded levels");
    for (auto& lev : m_loaded_tfrag3_levels) {
      auto lev_color = green;
      if (lev.second->frames_since_last_used > 0) {
        lev_color = blue;
      }
      if (lev.second->frames_since_last_used > 180) {
        lev_color = red;
      }
      ImGui::TextColored(lev_color, "%20s : %3d", lev.first.c_str(),
                         lev.second->frames_since_last_used);
      ImGui::Text("  %d textures", (int)lev.second->textures.size());
      ImGui::Text("  %d merc", (int)lev.second->merc_model_lookup.size());
    }
    ImGui::NewLine();
    ImGui::Separator();
  }

  ImGui::End();
}

/*!
 * Loader function that runs in a completely separate thread.
 * This is used for file I/O and unpacking.
 */
void Loader::loader_thread() {
  try {
    while (!m_want_shutdown) {
      prof().root_event();
      std::unique_lock<std::mutex> lk(m_loader_mutex);

      // this will keep us asleep until we've got a level to load.
      m_loader_cv.wait(lk, [&] { return !m_level_to_load.empty() || m_want_shutdown; });
      if (m_want_shutdown) {
        return;
      }
      std::string lev = m_level_to_load;
      // don't hold the lock while reading the file.
      lk.unlock();

      // simulate slower hard drive (so that the loader thread can lose to the game loads)
      // std::this_thread::sleep_for(std::chrono::milliseconds(1500));

      // load the fr3 file
      prof().begin_event("read-file");
      Timer disk_timer;
      auto data = file_util::read_binary_file(m_base_path / fmt::format("{}.fr3", lev));
      double disk_load_time = disk_timer.getSeconds();
      prof().end_event();

      // the FR3 files are compressed
      prof().begin_event("decompress-file");
      Timer decomp_timer;
      auto decomp_data = compression::decompress_zstd(data.data(), data.size());
      double decomp_time = decomp_timer.getSeconds();
      prof().end_event();

      // Read back into the tfrag3::Level structure
      prof().begin_event("deserialize");
      Timer import_timer;
      auto result = std::make_unique<tfrag3::Level>();
      Serializer ser(decomp_data.data(), decomp_data.size());
      result->serialize(ser);
      double import_time = import_timer.getSeconds();
      prof().end_event();

      // and finally "unpack", which creates the vertex data we'll upload to the GPU

      Timer unpack_timer;
      {
        auto p = scoped_prof("tie-unpack");
        for (auto& tie_tree : result->tie_trees) {
          for (auto& tree : tie_tree) {
            tree.unpack();
          }
        }
      }

      {
        auto p = scoped_prof("tfrag-unpack");
        for (auto& t_tree : result->tfrag_trees) {
          for (auto& tree : t_tree) {
            tree.unpack();
          }
        }
      }

      {
        auto p = scoped_prof("shrub-unpack");
        for (auto& shrub_tree : result->shrub_trees) {
          shrub_tree.unpack();
        }
      }

      fmt::print(
          "------------> Load from file: {:.3f}s, import {:.3f}s, decomp {:.3f}s unpack {:.3f}s\n",
          disk_load_time, import_time, decomp_time, unpack_timer.getSeconds());

      // grab the lock again
      lk.lock();
      // move this level to "initializing" state.
      m_initializing_tfrag3_levels[lev] = std::make_unique<LevelData>();  // reset load state
      m_initializing_tfrag3_levels[lev]->level = std::move(result);
      m_level_to_load = "";
      m_file_load_done_cv.notify_all();
    }
  } catch (std::exception& e) {
    ASSERT_MSG(false, fmt::format("Exception {} encountered in loader_thread", e.what()));
  }
}

/*!
 * Load a "common" FR3 file that has non-level textures.
 * This should be called during initialization, before any threaded loading goes on.
 */
const tfrag3::Level& Loader::load_common(TexturePool& tex_pool, const std::string& name) {
  auto data = file_util::read_binary_file(m_base_path / fmt::format("{}.fr3", name));

  auto decomp_data = compression::decompress_zstd(data.data(), data.size());
  Serializer ser(decomp_data.data(), decomp_data.size());
  m_common_level.level = std::make_unique<tfrag3::Level>();
  m_common_level.level->serialize(ser);
  for (auto& tex : m_common_level.level->textures) {
    m_common_level.textures.push_back(add_texture(tex_pool, tex, true));
  }

  Timer tim;
  MercLoaderStage mls;
  LoaderInput input;
  input.tex_pool = &tex_pool;
  input.mercs = &m_all_merc_models;
  input.lev_data = &m_common_level;
  input.buffers = &m_buffer_pool;
  bool done = false;
  while (!done) {
    done = mls.run(tim, input);
  }
  return *m_common_level.level;
}

bool Loader::upload_textures(Timer& timer, LevelData& data, TexturePool& texture_pool) {
  // try to move level from initializing to initialized:

  auto evt = scoped_prof("upload-textures");
  constexpr int MAX_TEX_BYTES_PER_FRAME = 1024 * 128;

  int bytes_this_run = 0;
  int tex_this_run = 0;
  if (data.textures.size() < data.level->textures.size()) {
    std::unique_lock<std::mutex> tpool_lock(texture_pool.mutex());
    while (data.textures.size() < data.level->textures.size()) {
      auto& tex = data.level->textures[data.textures.size()];
      data.textures.push_back(add_texture(texture_pool, tex, false));
      bytes_this_run += tex.w * tex.h * 4;
      tex_this_run++;
      if (tex_this_run > 20) {
        break;
      }
      if (bytes_this_run > MAX_TEX_BYTES_PER_FRAME || timer.getMs() > SHARED_TEXTURE_LOAD_BUDGET) {
        break;
      }
    }
  }
  return data.textures.size() == data.level->textures.size();
}

void Loader::update_blocking(TexturePool& tex_pool) {
  fmt::print("NOTE: coming out of blackout on next frame, doing all loads now...\n");

#ifdef __SWITCH__
  // FIX 33 (AI-assisted): free everything the game no longer holds BEFORE
  // staging the new area. The screen has been black, so recycling now is
  // invisible, and peak GPU memory becomes "new area" instead of
  // "old area + new area" (the combination that killed nouveau_mm during
  // zoomer area transitions).
  purge_retired_levels(tex_pool, true);
#endif

  bool missing_levels = true;
  while (missing_levels) {
    bool needs_run = true;

    while (needs_run) {
      needs_run = false;
      {
        std::unique_lock<std::mutex> lk(m_loader_mutex);
        if (!m_level_to_load.empty()) {
          m_file_load_done_cv.wait(lk, [&]() { return m_level_to_load.empty(); });
        }
      }
    }

    needs_run = true;

    while (needs_run) {
      needs_run = false;
      {
        std::unique_lock<std::mutex> lk(m_loader_mutex);
        if (!m_initializing_tfrag3_levels.empty()) {
          needs_run = true;
        }
      }

      if (needs_run) {
        update(tex_pool);
      }
    }

    {
      std::unique_lock<std::mutex> lk(m_loader_mutex);
      missing_levels = false;
      for (auto& des : m_desired_levels) {
        if (m_loaded_tfrag3_levels.find(des) == m_loaded_tfrag3_levels.end()) {
          fmt::print("blackout loader doing additional level {}...\n", des);
          missing_levels = true;
        }
      }
    }

    if (missing_levels) {
      set_want_levels(m_desired_levels);
    }
  }

  fmt::print("Blackout loads done. Current status:");
  std::unique_lock<std::mutex> lk(m_loader_mutex);
  for (auto& ld : m_loaded_tfrag3_levels) {
    fmt::print("  {} is loaded.\n", ld.first);
  }
}

/*!
 * Choose a level to evict, or nullptr if none is eligible.
 * FIX 33 (AI-assisted): on Switch the game tells us every frame which levels
 * it holds (__pc-set-levels -> m_desired_levels) and which it is actually
 * displaying (__pc-set-active-levels -> m_active_levels; see
 * goal_src/jak2/engine/level/level.gc). FIX 36: retired levels stay resident
 * for several seconds and are only recycled under real memory pressure, plus
 * a live-level cap keeps area transitions from piling up. Render thread only.
 */
const std::string* Loader::pick_eviction_victim() {
  std::unique_lock<std::mutex> lk(m_loader_mutex);
#ifdef __SWITCH__
  // FIX 36 Task 3 (AI-assisted): stop throwing away levels that are about to
  // be needed again. FIX 33 retired anything 30 frames off the want-list
  // while the buffer pool sat on 208 MB of free buffers - a city re-entry
  // then paid a full re-upload (tens of seconds at the old per-frame caps).
  constexpr int kRetiredAge = 300;  // frames off the game's want-list (~5-10 s)
  constexpr int kMaxLiveLevels = 8;
  const bool at_cap = (int)m_loaded_tfrag3_levels.size() >= kMaxLiveLevels;
  const bool low_mem = loader_under_pressure();
  const std::string* best = nullptr;
  int best_age = -1;
  for (auto& [name, lev] : m_loaded_tfrag3_levels) {
    if (std::find(m_active_levels.begin(), m_active_levels.end(), name) !=
        m_active_levels.end()) {
      continue;  // currently displayed - never recycle
    }
    if (std::find(m_desired_levels.begin(), m_desired_levels.end(), name) !=
        m_desired_levels.end()) {
      continue;  // the game still holds this level
    }
    const int age = lev->frames_since_last_used;
    if (((low_mem && age >= kRetiredAge) || at_cap) && age > best_age) {
      best_age = age;
      best = &name;
    }
  }
  return best;
#else
  // Desktop: legacy behavior - only unload once we're over m_max_levels, and
  // only levels unused for 180 frames, preferring ones no longer desired.
  if ((int)m_loaded_tfrag3_levels.size() < m_max_levels) {
    return nullptr;
  }
  for (auto& [name, lev] : m_loaded_tfrag3_levels) {
    if (lev->frames_since_last_used > 180 &&
        std::find(m_desired_levels.begin(), m_desired_levels.end(), name) ==
            m_desired_levels.end()) {
      return &name;
    }
  }

  for (const auto& [name, lev] : m_loaded_tfrag3_levels) {
    if (lev->frames_since_last_used > 180) {
      return &name;
    }
  }
  return nullptr;
#endif
}

/*!
 * Tear down every GPU object owned by a level: removes pool textures from the
 * TexturePool, queues GL textures for paced deletion, and returns all GL
 * buffers to the GpuBufferPool (FIX 33: recycling instead of delete/allocate
 * churn). Also drops the level's merc model references. Render thread only;
 * the caller is responsible for removing the LevelData itself.
 */
void Loader::unload_level_gpu_objects(LevelData& lev, TexturePool& tex_pool) {
  {
    std::unique_lock<std::mutex> lk(tex_pool.mutex());
    for (size_t i = 0; i < lev.textures.size() && i < lev.level->textures.size(); i++) {
      const auto& tex = lev.level->textures[i];
      if (tex.load_to_pool) {
        tex_pool.unload_texture(PcTextureId::from_combo_id(tex.combo_id), lev.textures[i]);
      }
    }
  }

  for (auto tex : lev.textures) {
    if (EXTRA_TEX_DEBUG) {
      for (auto& slot : tex_pool.all_textures()) {
        if (slot.source) {
          ASSERT(slot.gpu_texture != tex);
        } else {
          ASSERT(slot.gpu_texture != tex);
        }
      }
    }
    m_garbage_textures.push_back(tex);
  }

  // FIX 33: buffers go back to the pool instead of being deleted. This also
  // fixes the old normal-eviction path, which never released shrub buffers
  // or hfrag vertices (and queued hfrag indices twice - a double delete).
  for (auto& tie_geo : lev.tie_data) {
    for (auto& tie_tree : tie_geo) {
      m_buffer_pool.release(tie_tree.vertex_buffer);
      if (tie_tree.has_wind) {
        m_buffer_pool.release(tie_tree.wind_indices);
      }
      m_buffer_pool.release(tie_tree.index_buffer);
    }
  }
  for (auto& tfrag_geo : lev.tfrag_vertex_data) {
    for (auto& buf : tfrag_geo) {
      m_buffer_pool.release(buf);
    }
  }
  for (auto& buf : lev.shrub_vertex_data) {
    m_buffer_pool.release(buf);
  }
  m_buffer_pool.release(lev.hfrag_indices);
  m_buffer_pool.release(lev.hfrag_vertices);
  m_buffer_pool.release(lev.collide_vertices);
  m_buffer_pool.release(lev.merc_vertices);
  m_buffer_pool.release(lev.merc_indices);

  for (auto& model : lev.level->merc_data.models) {
    auto it = m_all_merc_models.find(model.name);
    if (it == m_all_merc_models.end()) {
      continue;
    }
    MercRef ref{&model, lev.load_id};
    auto ref_it = std::ranges::find(it->second, ref);
    if (ref_it != it->second.end()) {
      it->second.erase(ref_it);
    }
  }
}

/*!
 * Delete every queued garbage texture right now. Used by the blackout purge,
 * where we want the memory back before the next area stages (FIX 33).
 */
void Loader::flush_texture_garbage() {
  for (auto tex : m_garbage_textures) {
    glDeleteTextures(1, &tex);
  }
  m_garbage_textures.clear();
}

/*!
 * Recycle every level the game no longer holds, i.e. not in the want-list
 * (__pc-set-levels) and not displayed (__pc-set-active-levels). Called at
 * the end of a blackout (update_blocking) so the new area is staged into
 * freed space instead of on top of the old one. With `immediate`, also
 * flushes the garbage queues and glFinish()es so the driver has actually
 * reclaimed the memory before the new allocations start. (FIX 33)
 */
void Loader::purge_retired_levels(TexturePool& tex_pool, bool immediate) {
  std::vector<std::string> victims;
  {
    std::unique_lock<std::mutex> lk(m_loader_mutex);
    for (auto& [name, lev] : m_loaded_tfrag3_levels) {
      const bool active = std::find(m_active_levels.begin(), m_active_levels.end(), name) !=
                          m_active_levels.end();
      const bool desired = std::find(m_desired_levels.begin(), m_desired_levels.end(), name) !=
                           m_desired_levels.end();
      if (!active && !desired) {
        victims.push_back(name);
      }
    }
  }
  if (victims.empty()) {
    return;
  }
  fmt::print("[loader] blackout purge: recycling {} retired level(s)\n", victims.size());
  for (const auto& name : victims) {
    std::unique_ptr<LevelData> lev;
    {
      std::unique_lock<std::mutex> lk(m_loader_mutex);
      auto it = m_loaded_tfrag3_levels.find(name);
      if (it == m_loaded_tfrag3_levels.end()) {
        continue;
      }
      lev = std::move(it->second);
      m_loaded_tfrag3_levels.erase(it);
    }
    fmt::print("[loader]   purging {}\n", name);
    unload_level_gpu_objects(*lev, tex_pool);
  }
  if (immediate) {
    flush_texture_garbage();
    for (auto buf : m_garbage_buffers) {
      glDeleteBuffers(1, &buf);
    }
    m_garbage_buffers.clear();
    glFinish();
  }
}

void Loader::update(TexturePool& texture_pool) {
  Timer loader_timer;

#ifdef __SWITCH__
  // FIX 36 Task 3 (AI-assisted): retune the loader budget from the measured
  // frame gap and the blackout flag (set by OpenGLRenderer). Must run before
  // the stages consume g_loader_budget below.
  update_frame_budget();
  // FIX 33 (AI-assisted): periodic loader pressure telemetry, so we can see
  // live levels / pooled buffer usage from gk_stdout.txt on the console.
  if (++m_stats_frame_count >= 120) {
    m_stats_frame_count = 0;
    size_t live, init, want;
    {
      std::unique_lock<std::mutex> lk(m_loader_mutex);
      live = m_loaded_tfrag3_levels.size();
      init = m_initializing_tfrag3_levels.size();
      want = m_desired_levels.size();
    }
    fmt::print(
        "[loader] live={} init={} want={} | pool={} bufs {:.1f}MB free, {} out | gc {} tex {} "
        "buf | budget {} (ema {:.1f}ms)\n",
        live, init, want, m_buffer_pool.pooled_buffers(),
        (double)m_buffer_pool.pooled_bytes() / (1024.0 * 1024.0),
        m_buffer_pool.outstanding_buffers(), m_garbage_textures.size(),
        m_garbage_buffers.size(), m_budget_mode, m_frame_gap_ema_ms);
  }
#endif

  if (m_want_reload) {
    std::unique_lock lk(m_loader_mutex);
    if (m_level_to_load.empty() && m_initializing_tfrag3_levels.empty()) {
      m_want_reload = false;
      lk.unlock();
      do_reload(texture_pool);
      return;
    }
  }

  if (!m_single_level_to_reload.empty()) {
    std::unique_lock lk(m_loader_mutex);
    bool in_init = m_initializing_tfrag3_levels.count(m_single_level_to_reload) > 0;
    if (m_level_to_load.empty() && !in_init) {
      std::string name = std::move(m_single_level_to_reload);
      m_single_level_to_reload.clear();
      lk.unlock();
      do_reload_level(name, texture_pool);
      return;
    }
  }

  if (m_want_reload_common) {
    std::unique_lock lk(m_loader_mutex);
    bool in_init = m_initializing_tfrag3_levels.count(m_single_level_to_reload) > 0;
    if (m_level_to_load.empty() && !in_init) {
      m_want_reload_common = false;
      lk.unlock();
      do_reload_common(texture_pool);
      return;
    }
  }

  {
    // lock because we're accessing m_active_levels
    std::unique_lock<std::mutex> lk(m_loader_mutex);
    // only main thread can touch this.
    for (auto& [name, lev] : m_loaded_tfrag3_levels) {
      if (std::find(m_active_levels.begin(), m_active_levels.end(), name) ==
          m_active_levels.end()) {
        lev->frames_since_last_used++;
      } else {
        lev->frames_since_last_used = 0;
      }
    }
  }

  // work on moving initializing to initialized.
  {
    // accessing initializing, should lock
    std::unique_lock<std::mutex> lk(m_loader_mutex);
    // grab the first initializing level:
    const auto& it = m_initializing_tfrag3_levels.begin();
    if (it != m_initializing_tfrag3_levels.end()) {
      std::string name = it->first;
      auto& lev = it->second;
      if (it->second->load_id == UINT64_MAX) {
        it->second->load_id = m_id++;
      }

      // we're the only place that erases, so it's okay to unlock and hold a reference
      lk.unlock();
      bool done = true;
      LoaderInput loader_input;
      loader_input.lev_data = lev.get();
      loader_input.mercs = &m_all_merc_models;
      loader_input.tex_pool = &texture_pool;
      loader_input.buffers = &m_buffer_pool;

      for (auto& stage : m_loader_stages) {
        auto evt = scoped_prof(fmt::format("stage-{}", stage->name()).c_str());
        Timer stage_timer;
        done = stage->run(loader_timer, loader_input);
        if (stage_timer.getMs() > 5.f) {
          fmt::print("stage {} took {:.2f} ms\n", stage->name(), stage_timer.getMs());
        }
        if (!done) {
          break;
        }
      }

      if (done) {
        auto evt = scoped_prof("finish-stages");
        lk.lock();
        m_loaded_tfrag3_levels[name] = std::move(lev);
        m_initializing_tfrag3_levels.erase(it);
#ifdef __SWITCH__
        // FIX 36 Task 3 (AI-assisted): load-completion timing, so cold entry
        // vs re-entry can be compared from the log (brief §4.4). The clock
        // starts in set_want_levels(), under this same mutex.
        if (auto st = m_load_start.find(name); st != m_load_start.end()) {
          const double secs =
              std::chrono::duration<double>(std::chrono::steady_clock::now() - st->second).count();
          m_load_start.erase(st);
          fmt::print("[loader] level {} ready in {:.2f}s (budget {})\n", name, secs, m_budget_mode);
        }
#endif

        for (auto& stage : m_loader_stages) {
          stage->reset();
        }
      }
    }
  }

  // ---- FIX 33 (AI-assisted): level recycling + garbage management ----
  // The old code only unloaded levels when the loader was otherwise idle and
  // only after 180 unused frames. During area transitions (blackout loads)
  // the loader is never idle, so every level ever visited stayed resident:
  // peak GPU memory became "old area + new area" and the Tegra suballocator
  // (nouveau_mm) aborted during a large glBufferData. Now eviction runs
  // every frame, at most one level at a time, following the game's own
  // hints - see pick_eviction_victim().
  {
    auto evt = scoped_prof("gpu-unload");
    Timer unload_timer;
    const std::string* to_unload = pick_eviction_victim();
    if (to_unload) {
      std::string victim_name = *to_unload;
      std::unique_ptr<LevelData> lev;
      {
        std::unique_lock<std::mutex> lk(m_loader_mutex);
        auto it = m_loaded_tfrag3_levels.find(victim_name);
        if (it != m_loaded_tfrag3_levels.end()) {
          lev = std::move(it->second);
          m_loaded_tfrag3_levels.erase(it);
        }
      }
      if (lev) {
        fmt::print("------------------------- PC unloading {}\n", victim_name);
        unload_level_gpu_objects(*lev, texture_pool);
      }
    }
    if (unload_timer.getMs() > 5.f) {
      fmt::print("Unload took {:.2f}ms\n", unload_timer.getMs());
    }
  }

  // FIX 33: always drain a little GL garbage, even while another level is
  // staging. The old code only drained when the loader was idle, so a busy
  // loader could never actually free its deleted textures/buffers.
  {
    auto evt = scoped_prof("garbage");
    for (int i = 0; i < 5 && !m_garbage_buffers.empty(); i++) {
      glDeleteBuffers(1, &m_garbage_buffers.back());
      m_garbage_buffers.pop_back();
    }
    for (int i = 0; i < 20 && !m_garbage_textures.empty(); i++) {
      glDeleteTextures(1, &m_garbage_textures.back());
      m_garbage_textures.pop_back();
    }
  }

  if (loader_timer.getMs() > 5) {
    fmt::print("Loader::update slow setup: {:.1f}ms\n", loader_timer.getMs());
  }
}

std::optional<MercRef> Loader::get_merc_model(const char* model_name) {
  // don't think we need to lock here...
  const auto& it = m_all_merc_models.find(model_name);
  if (it != m_all_merc_models.end() && !it->second.empty()) {
    // it->second.front().parent_level->frames_since_last_used = 0;
    return it->second.front();
  } else {
    return std::nullopt;
  }
}

void Loader::do_reload_level(const std::string& name, TexturePool& texture_pool) {
  std::unique_ptr<LevelData> lev;
  {
    std::unique_lock<std::mutex> lk(m_loader_mutex);
    auto it = m_loaded_tfrag3_levels.find(name);
    if (it == m_loaded_tfrag3_levels.end()) {
      return;
    }
    lev = std::move(it->second);
    m_loaded_tfrag3_levels.erase(it);
  }
  fmt::print("force reload: unloading {}\n", name);
  // FIX 33: shared unload path - the level's buffers return to the pool and
  // get reused when it reloads.
  unload_level_gpu_objects(*lev, texture_pool);

  std::unique_lock lk(m_loader_mutex);
  if (m_level_to_load.empty()) {
    m_level_to_load = name;
    lk.unlock();
    m_loader_cv.notify_all();
  }
}

void Loader::do_reload_common(TexturePool& tex_pool) {
  fmt::print("loader: force reloading common level\n");
  {
    std::unique_lock lk(tex_pool.mutex());
    for (size_t i = 0;
         i < m_common_level.textures.size() && i < m_common_level.level->textures.size(); i++) {
      auto& tex = m_common_level.level->textures[i];
      if (tex.load_to_pool) {
        tex_pool.unload_texture(PcTextureId::from_combo_id(tex.combo_id),
                                m_common_level.textures[i]);
      }
    }
  }
  for (auto tex : m_common_level.textures) {
    glDeleteTextures(1, &tex);
  }
  // FIX 33: return the common level's merc buffers to the pool instead of
  // leaking them on every common reload.
  m_buffer_pool.release(m_common_level.merc_vertices);
  m_buffer_pool.release(m_common_level.merc_indices);
  for (auto& model : m_common_level.level->merc_data.models) {
    auto it = m_all_merc_models.find(model.name);
    if (it == m_all_merc_models.end())
      continue;
    MercRef ref{&model, m_common_level.load_id};
    auto ref_it = std::ranges::find(it->second, ref);
    if (ref_it != it->second.end())
      it->second.erase(ref_it);
  }
  m_common_level = LevelData{};
  load_common(tex_pool, "GAME");
}

void Loader::do_reload(TexturePool& texture_pool) {
  fmt::print("loader: force reloading all levels\n");
  // FIX 33: extract all levels first, then tear down their GPU objects
  // through the shared path (buffers return to the pool).
  std::vector<std::unique_ptr<LevelData>> levels;
  {
    std::unique_lock<std::mutex> lk(m_loader_mutex);
    levels.reserve(m_loaded_tfrag3_levels.size());
    for (auto& [name, lev] : m_loaded_tfrag3_levels) {
      levels.push_back(std::move(lev));
    }
    m_loaded_tfrag3_levels.clear();
  }
  for (auto& lev : levels) {
    unload_level_gpu_objects(*lev, texture_pool);
  }

  for (auto buf : m_garbage_buffers)
    glDeleteBuffers(1, &buf);
  m_garbage_buffers.clear();
  flush_texture_garbage();
  // A full reload wants a clean slate: actually delete the pooled buffers
  // instead of keeping them around for reuse (FIX 33).
  m_buffer_pool.clear();

  set_want_levels(m_desired_levels);
}