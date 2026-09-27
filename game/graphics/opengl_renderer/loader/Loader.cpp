#include "Loader.h"

#include <algorithm>
#include <ranges>
#include <cstring>

#include "common/global_profiler/GlobalProfiler.h"
#include "common/util/FileUtil.h"
#include "common/util/Timer.h"
#include "common/util/compress.h"

#include "game/graphics/opengl_renderer/loader/LoaderStages.h"

// FIX 55 (AI-assisted): for switch_diag_enabled(), which gates the gpu probe. Off-Switch this
// header supplies an always-true stub, so the gate is inert on host builds.
#include "game/switch/run_log.h"

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
#ifdef __SWITCH__
      // FIX 58 (AI-assisted): before paying a file load plus a full GPU
      // re-upload, check the warm cache. A hit moves the level straight back
      // to live with every texture/buffer/pool registration still valid, so
      // re-entering a city section no longer re-uploads lwidea's 1222
      // textures over ~12 seconds. Keep scanning afterwards - revivals are
      // free, so revive every wanted level we can in this one call.
      if (revive_from_cache(lev)) {
        continue;
      }
#endif
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

  // FIX 52 (AI-assisted): THE ONE TUNING KNOB THAT COULD NOT BE TUNED BEFORE.
  //
  // The tiers below pick a budget from the frame-gap EMA, which times the whole frame and
  // is therefore dominated by the renderer. Two consecutive sessions of loader work
  // (FIX 47-49) were spent adjusting those numbers while the logs showed
  // `[phase] loader 0.01 | pcrtc 22.33`, i.e. the loader's own attributed cost was
  // one hundredth of a millisecond. The signal was never about the loader.
  //
  // The texture budget governs identical work however it is chosen, so it is safe -- and
  // correct -- to shrink it when the loader's *own measured* GPU cost is high. This does
  // not undo FIX 38 ("backlog beats frame time"): the floor is untouched, and the tier is
  // only ever lowered by evidence that the loader itself is what the GPU is stuck on.
  //
  // Note the asymmetry with FIX 49, which is the whole reason this is not a repeat of it:
  // FIX 49 clamped the MIP RATE against a wall-clock budget the upload had already
  // exceeded, so the clamp was permanently true and the queue grew to 768. This clamps
  // the TEXTURE BYTE CAP against a measured GPU cost, and only while the GPU is
  // demonstrably saturated by this loader -- if the cost is low, nothing changes at all.
  double gpu_scale = 1.0;
  if (m_loader_gpu_ema_ms > 12.0) {
    gpu_scale = 0.25;  // the loader alone is eating >12 ms of GPU per frame
  } else if (m_loader_gpu_ema_ms > 6.0) {
    gpu_scale = 0.5;   // significant; slow down before it becomes the stall it looks like
  }

  // FIX 39 (AI-assisted): a blackout is a loading screen and a backlog is a stream-in;
  // both are windows where resolution is worth trading for load speed.
  loadboost_set_streaming(m_blackout || pending > 0);

  LoaderFrameBudget want;
  const char* mode;
  if (m_blackout) {
    want = {12.f, 4 * 1024 * 1024, 4096};
    mode = "blackout";
  } else if (pending > 0) {
    // FIX 46b (AI-assisted): CATCH-UP MUST NOT DEEPEN A DROPPED FRAME.
    //
    // FIX 38 fixed the death spiral where a bad frame rate throttled the loader
    // to nothing. But it overcorrected in the other direction: `catchup` (8 ms /
    // 2 MB) was handed out whenever the EMA was <= 45 ms, i.e. *including* the
    // case where the frame is already at 30 fps and owes nothing. On the Switch
    // the frame is ~32 ms of a 33.3 ms budget with zero slack (FIX 43), so 8 ms
    // of loader work does not fit in the slack -- it is simply added to the
    // frame. That is the "slow motion while an area loads", and it is why
    // lowering the resolution (LoadBoost) never helped: the frame is not
    // pixel-bound.
    //
    // The floor stays (loading must never be throttled to a crawl again - that
    // was FIX 38's whole point), but above the floor we now only take more when
    // the frame demonstrably has room. A frame at 30 fps keeps the *floor*
    // instead of the larger budget, so the extra work is spread over more frames
    // rather than injected into one.
    //
    // Threshold is 30 fps (33.3 ms) with a little tolerance, not the old 45 ms:
    // 45 ms is already a dropped frame in a 30 fps target, which is far too late
    // to start being careful.
    constexpr double kFrameHasRoomMs = 30.0;
    constexpr double kFrameIsDroppedMs = 38.0;
    // FIX 50 (AI-assisted): ON SWITCH, STOP KEYING THE LOADER OFF FRAME TIME ALONE.
    //
    // The 2026-09-27 hardware log finally separated the two costs:
    //
    //   [phase] setup 0.01 | loader 0.01 | buckets 32.31 | blit 0.00 | bucket-sum 30.94
    //   [loader] level lwidea ready in 19.88s (budget catchup-floor)
    //   [loader] tex stage: 1222 textures, upload 2203.1ms
    //
    // The renderer was spending ~32 ms/frame (mostly the blit stall FIX 50 removes),
    // which pushed the frame-gap EMA over 38 ms, which pinned the loader to
    // `catchup-floor` (4 ms/frame) for the entire city. So a 19.9 s load was spent
    // uploading textures at 4 ms/frame while the loader's own measured cost was
    // 0.01 ms - the frame time was almost entirely the renderer's, and throttling
    // the loader could never have helped it. FIX 38's rule ("backlog beats frame
    // time") was being defeated by a renderer problem the loader cannot see.
    //
    // The floor still exists and the EMA still matters - loading must never go back
    // to being throttled to a crawl (FIX 38), but "the frame is slow" is no longer
    // sufficient reason to starve the loader when there is a backlog. Raise the
    // floor so a real load progresses at a useful rate, and keep the larger tiers
    // for frames that genuinely have room.
    if (m_frame_gap_ema_ms > kFrameIsDroppedMs) {
      want = {8.f, 2 * 1024 * 1024, 2048};
      mode = "catchup-floor";
    } else if (m_frame_gap_ema_ms > kFrameHasRoomMs) {
      // Hitting the 30 fps target: modest, so the upload still fits the frame.
      want = {5.f, 1024 * 1024, 1024};
      mode = "catchup-pace";
    } else {
      want = {8.f, 2 * 1024 * 1024, 2048};
      mode = "catchup";
    }
    // FIX 52 (AI-assisted): apply the measured-GPU scaling to the texture byte cap of the
    // tier just selected. Applied to tex_bytes and stage_kb together, because they govern
    // the same uploads -- scaling only one would just move the ceiling.
    //
    // The floor is a floor: `catchup-floor` exists because FIX 38 established that a slow
    // frame must not starve a real backlog, so scaling it down would re-introduce the exact
    // death spiral FIX 38 removed. Slow the *faster* tiers instead; the floor already is the
    // conservative case.
    if (gpu_scale < 1.0 && std::strcmp(mode, "catchup-floor") != 0) {
      want.tex_bytes = (u32)((double)want.tex_bytes * gpu_scale);
      want.stage_kb = (u32)std::max(256.0, (double)want.stage_kb * gpu_scale);
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
  // FIX 52 (AI-assisted): the `|| want.tex_bytes != ...` clause is load-bearing. This
  // block only publishes `want` to the stages when the mode NAME changes, and the FIX 52
  // GPU scaling changes the byte cap *within* a mode. Without the extra clause a
  // GPU-driven rescale would be computed every frame and then silently discarded -- the
  // exact class of bug where the instrumentation looks healthy and nothing happens.
  if (std::strcmp(mode, m_budget_mode) != 0 || want.tex_bytes != g_loader_budget.tex_bytes) {
    m_budget_mode = mode;
    g_loader_budget = want;
    // FIX 46 (AI-assisted): also report the live-level cap and how many levels
    // are resident/held/wanted. The FIX 46 theory is that the loader used to be
    // capped below what the game holds (jak3 asks for 11, cap was 8), so it
    // evicted a level the game still wanted every frame and re-uploaded it.
    // "live" pinned at the cap together with "hold" above it would prove the
    // churn; after this fix "live" should be able to reach "hold" and stay.
    fmt::print(
        "[loader] budget ms={:.1f} tex_kb={} mode={} (ema {:.1f}ms, pending {}, live {}/{}, "
        "hold {}, active {})\n",
        (double)g_loader_budget.ms, g_loader_budget.tex_bytes / 1024, mode, m_frame_gap_ema_ms,
        pending, (int)m_loaded_tfrag3_levels.size(), max_live_levels(), (int)m_desired_levels.size(),
        (int)m_active_levels.size());
  }
}

/*!
 * FIX 52 (AI-assisted): MEASURE THE LOADER'S GPU COST, NOT ITS SUBMIT TIME.
 *
 * Why this exists. Every loader budget in this file is keyed off a signal that cannot
 * see the loader's own cost:
 *
 *   m_frame_gap_ema_ms  times the whole frame, so it is dominated by the renderer.
 *   loader_timer        times the GL *calls*, which only enqueue work. glTexSubImage2D
 *                       and glGenerateMipmap return as soon as the command is in the
 *                       driver's queue; the GPU does the work later, during the
 *                       swapchain acquire that OpenGLRenderer.cpp times as `pcrtc`.
 *
 * So these three lines can all appear in the same second, none of them wrong, and none
 * of them the loader's real cost:
 *
 *   [phase] loader 0.01 | pcrtc 22.33      <- loader attributed 0.01 ms
 *   Loader::update slow setup: 20.1ms      <- submit time, 10x its own budget
 *   [loader] budget ... mode=catchup-pace  <- decided from a number it cannot attribute
 *
 * This is exactly why FIX 49's clamp regressed: it throttled a number that measured
 * nothing, so the mip queue grew to 768 while the frame showed no improvement.
 *
 * The fix is to insert a fence after the loader submits its frame's work and wait for
 * it. That wait is a true "how long did my work take" measurement -- the first the
 * loader has ever had.
 *
 * Cost control, because a fence that waits forever would itself be the stall:
 *  - One fence object is reused across frames (m_gpu_fence), never one per frame.
 *  - The wait is bounded by kProbeTimeoutMs. On timeout the result is reported as a
 *    lower bound (negative) rather than blocking the frame; a timeout is itself the
 *    finding, meaning the GPU is deeper behind than the timeout.
 *  - Results are smoothed with the same 1/8 alpha as the frame-gap EMA, so a single
 *    slow frame cannot swing the budget.
 *
 * Render thread only.
 */
double Loader::gpu_cost_probe() {
  // FIX 55 (AI-assisted): the probe is self-gating. It was added to answer one question --
  // "is the loader's GPU cost the 20ms in the frame gap?" -- and the answer was no: 1675 of
  // 1868 readings were 0, and every real reading was under 3.7ms. Keeping it armed costs a
  // glFenceSync + glClientWaitSync every frame, which is the trade FIX 40 rejected when it
  // made diagnostics opt-in.
  //
  // It also outlived its own baseline: `gpu=` prints through fmt::print and so reports
  // unconditionally, but the `[phase]` split it has to be read against is gated on
  // switch_diag_enabled() and is OFF by default -- which is how two sessions of loader work
  // ran without anyone ever seeing the frame split. Tie the probe to the same switch that
  // gates the number it explains, so "no gpu= lines" and "no [phase] lines" mean the same
  // thing and cannot be misread as a finding.
  //
  // This sits above the submits check on purpose: gating has to hold on every path, or a
  // frame with nothing to measure would leave a fence armed and the probe would quietly
  // resume the moment the loader next submitted anything.
  if (!switch_diag_enabled()) {
    if (m_gpu_fence) {
      if (glad_glDeleteSync) {
        glad_glDeleteSync((GLsync)m_gpu_fence);
      }
      m_gpu_fence = nullptr;
    }
    m_loader_gpu_last_ms = 0.0;
    return 0.0;
  }

  // FIX 52: the texture stage counts its submissions in a global (g_loader_gpu_submits_
  // this_frame) so both upload paths are covered without either stage knowing about the
  // probe. Take the count and clear it: it is "since the last probe", not "since boot".
  //
  // Nothing submitted means nothing to attribute, and an idle loader must not pay for a
  // measurement of zero. Decay rather than snap, so a load that just finished does not
  // keep governing the budget for several seconds afterwards.
  if (g_loader_gpu_submits_this_frame == 0) {
    m_loader_gpu_ema_ms *= 0.875;
    m_loader_gpu_last_ms = 0.0;
    return 0.0;
  }
  g_loader_gpu_submits_this_frame = 0;

  // The entry points live in glad's GL 3.2 block, which the Switch loader skips for the
  // "OpenGL ES 3.1" version string -- they are resolved by name in
  // graphics/pipelines/opengl.cpp (FIX 52 there). If resolution failed on some driver,
  // report the absence once rather than jumping to address 0.
  static bool s_reported_missing = false;
  if (!glad_glFenceSync || !glad_glClientWaitSync) {
    if (!s_reported_missing) {
      s_reported_missing = true;
      fmt::print("[loader] gpu-probe UNAVAILABLE: glFenceSync/glClientWaitSync are null\n");
    }
    return 0.0;
  }

  // Wait for the fence from the PREVIOUS frame. It was inserted after that frame's
  // loader submission, so its completion is that frame's loader GPU work retiring.
  double measured = 0.0;
  bool timed_out = false;
  if (m_gpu_fence) {
    constexpr double kProbeTimeoutMs = 8.0;
    const GLuint64 timeout_ns = (GLuint64)(kProbeTimeoutMs * 1e6);
    Timer wait_timer;
    // GL_SYNC_FLUSH_COMMANDS_BIT guarantees the fence is actually reached, so the wait
    // measures the work rather than sitting on a command that was never flushed.
    const GLenum r = glClientWaitSync((GLsync)m_gpu_fence, GL_SYNC_FLUSH_COMMANDS_BIT, timeout_ns);
    measured = wait_timer.getMs();
    timed_out = (r == GL_TIMEOUT_EXPIRED);
    glDeleteSync((GLsync)m_gpu_fence);
    m_gpu_fence = nullptr;
  }

  // Insert this frame's fence now that the previous one has been consumed.
  m_gpu_fence = (void*)glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  // FIX 54 (AI-assisted): validate before trusting it. A GLsync is an opaque pointer and
  // glFenceSync returns 0 on failure; some ES drivers also hand back a non-null value that
  // glIsSync rejects. Passing a bad object to glClientWaitSync is undefined, so check both
  // and drop the probe -- and the fence -- if it is not a real sync object. Without this,
  // a driver that cannot do fence sync would have the loader waiting on garbage pointers
  // every frame, which is a plausible way to reach the black screen this build produced.
  if (m_gpu_fence && glad_glIsSync && !glad_glIsSync((GLsync)m_gpu_fence)) {
    m_gpu_fence = nullptr;
  }
  if (!m_gpu_fence) {
    if (!s_reported_missing) {
      s_reported_missing = true;
      fmt::print("[loader] gpu-probe UNAVAILABLE: glFenceSync returned an invalid sync\n");
    }
    m_loader_gpu_last_ms = 0.0;
    return 0.0;
  }

  if (m_gpu_fence) {
    m_loader_gpu_ema_ms += (measured - m_loader_gpu_ema_ms) * 0.125;
  }

  // Negative marks a lower bound, so "8.0" and "8.0 and still counting" are
  // distinguishable in the log -- the difference decides whether the budget is helping
  // or merely guessing. See the slow-setup format string.
  m_loader_gpu_last_ms = timed_out ? -measured : measured;
  return measured;
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

      // FIX 48 (AI-assisted): this call is now a deliberate no-op. FIX 47 primed a
      // byte-swapped copy here; the swap turned out to be both wrong and
      // unnecessary, so there is nothing to stage and this loop is kept only so
      // the call site is obvious if staging is ever needed again.
      {
        auto p = scoped_prof("prime-texture-swap");
        Timer prime_timer;
        for (const auto& tex : result->textures) {
          prime_texture_swap(tex);
        }
        if (prime_timer.getMs() > 1.f) {
          fmt::print("[loader] FIX 48 primed {} texture swaps in {:.1f}ms (no-op)\n",
                     result->textures.size(), prime_timer.getMs());
        }
      }

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

// FIX 57 (AI-assisted): Loader::upload_textures was dead code (no callers; the
// live texture path is TextureLoaderStage in LoaderStages.cpp) and still
// carried the buggy FIX 46c "count cap before budget" pattern. Deleted so the
// pattern can't be copy-pasted back.

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

  // -------------------------------------------------------------------------
  // FIX 46 (AI-assisted): THE CAP WAS BELOW WHAT THE GAME LEGITIMATELY HOLDS.
  //
  // This was a flat 8. But the game can ask for more levels than that at once:
  //   jak1 LEVEL_TOTAL 3, jak2 LEVEL_TOTAL 7, jak3 LEVEL_TOTAL 11, jakx 11
  // (common/goal_constants.h; Loader is constructed with that value as
  // m_max_levels). So in jak3 the loader was structurally forbidden from
  // holding what GOAL kept resident, and `at_cap` alone -- with NO age test --
  // was enough to evict. Every frame the game asked for its 11 levels, the
  // loader held 8, evicted the oldest, the game re-requested it on the next
  // frame, and it was re-read, re-decompressed, re-unpacked and re-uploaded
  // from scratch. That is the permanent "huge slowdown" on city entry and the
  // reason an area could appear never to finish loading: the work was being
  // thrown away and redone, not merely delayed. A city holds the most levels,
  // which is why the cities were worst.
  //
  // The cap now derives from what the game can actually request, plus a little
  // slack for the level being staged. And `at_cap` no longer evicts on its
  // own: a level the game still holds (m_desired_levels) is skipped above, so
  // the only remaining cap victims are ones the game has genuinely dropped --
  // which the age test already gates.
  // -------------------------------------------------------------------------
  const bool at_cap = (int)m_loaded_tfrag3_levels.size() >= max_live_levels();
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
    // FIX 46: over the cap we may reclaim sooner than kRetiredAge, but never a
    // level younger than kMinReclaimAge -- otherwise a burst of requests could
    // evict something the game is about to want back (the churn above).
    constexpr int kMinReclaimAge = 60;  // ~1-2 s
    if (((low_mem && age >= kRetiredAge) || (at_cap && age >= kMinReclaimAge)) &&
        age > best_age) {
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

#ifdef __SWITCH__
  // FIX 59: park the objects in the size-keyed recycler instead of the delete
  // queue - see the long comment in LoaderStages.cpp. lev.textures is
  // index-aligned with lev.level->textures (both append in add_texture
  // order), which is where the (w,h) key comes from. No param re-normalising
  // is needed here: add_texture() sets MAX_LEVEL=0 + anisotropy on every
  // upload, and stale mip levels are regenerated by mipq as usual.
  ASSERT(lev.textures.size() == lev.level->textures.size());
  for (size_t i = 0; i < lev.textures.size(); i++) {
    texobj_release(lev.textures[i], lev.level->textures[i].w, lev.level->textures[i].h);
  }
#else
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
#endif

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

#ifdef __SWITCH__
// ---------------------------------------------------------------------------
// FIX 58 (AI-assisted): retired-level warm cache. See Loader.h for the
// rationale. Cache bounds: a full city loop retires ~6 levels / ~142MB of
// textures (hardware-measured), so a 6-level / 256MB cap holds lwidea +
// ctywide + every section on the route while staying a small fraction of
// the memory the 9-level live cap already allows.
namespace {
// FIX 58b: hardware logs showed a full city loop retires ~6 levels totalling
// ~142MB of textures (atoll 12, atollext 45, lwidea 49.5, ctywide 4.5,
// ctyslumc 5.7, ruins 25.1) - comfortably inside the byte cap, so the count
// cap is the practical bound and 4 was one section short of a full loop.
constexpr size_t kRetiredMaxLevels = 6;
constexpr size_t kRetiredMaxTexBytes = 256u * 1024 * 1024;
}  // namespace

void Loader::retire_to_cache(const std::string& name, std::unique_ptr<LevelData> lev,
                             TexturePool& tex_pool) {
  // Estimate the GPU texture footprint we are keeping alive: RGBA8 pixels plus
  // ~1/3 for the mip chain. Buffers are deliberately excluded - the
  // GpuBufferPool holds on to that memory for reuse either way, so keeping
  // them with the level is not extra cost.
  size_t bytes = 0;
  for (const auto& tex : lev->level->textures) {
    bytes += tex.data.size() * sizeof(u32);
  }
  lev->cached_tex_bytes = bytes + bytes / 3;

  size_t cache_levels, cache_bytes;
  {
    std::unique_lock<std::mutex> lk(m_loader_mutex);
    m_retired_tex_bytes += lev->cached_tex_bytes;
    m_retired_lru.push_back(name);
    m_retired_levels[name] = std::move(lev);
    cache_levels = m_retired_levels.size();
    cache_bytes = m_retired_tex_bytes;
  }
  fmt::print(
      "[fix58] retired {} to warm cache ({:.1f}MB tex kept; cache {} levels, {:.1f}MB)\n", name,
      (double)(bytes + bytes / 3) / (1024.0 * 1024.0), cache_levels,
      (double)cache_bytes / (1024.0 * 1024.0));

  // Enforce the caps: exceed them and the oldest entries get a real unload.
  while (true) {
    bool over;
    {
      std::unique_lock<std::mutex> lk(m_loader_mutex);
      over = m_retired_levels.size() > kRetiredMaxLevels ||
             m_retired_tex_bytes > kRetiredMaxTexBytes;
    }
    if (!over || !drop_oldest_retired(tex_pool)) {
      break;
    }
  }
}

bool Loader::drop_oldest_retired(TexturePool& tex_pool) {
  std::string name;
  std::unique_ptr<LevelData> lev;
  {
    std::unique_lock<std::mutex> lk(m_loader_mutex);
    if (m_retired_lru.empty()) {
      return false;
    }
    name = m_retired_lru.front();
    m_retired_lru.erase(m_retired_lru.begin());
    auto it = m_retired_levels.find(name);
    if (it == m_retired_levels.end()) {
      return false;
    }
    m_retired_tex_bytes -= it->second->cached_tex_bytes;
    lev = std::move(it->second);
    m_retired_levels.erase(it);
  }
  // GL teardown outside the loader mutex, same discipline as the normal
  // eviction path in update().
  fmt::print("------------------------- PC unloading {} (warm cache evict)\n", name);
  unload_level_gpu_objects(*lev, tex_pool);
  return true;
}

bool Loader::drop_retired_level(const std::string& name, TexturePool& tex_pool) {
  std::unique_ptr<LevelData> lev;
  {
    std::unique_lock<std::mutex> lk(m_loader_mutex);
    auto it = m_retired_levels.find(name);
    if (it == m_retired_levels.end()) {
      return false;
    }
    m_retired_tex_bytes -= it->second->cached_tex_bytes;
    lev = std::move(it->second);
    m_retired_levels.erase(it);
    auto lru_it = std::ranges::find(m_retired_lru, name);
    if (lru_it != m_retired_lru.end()) {
      m_retired_lru.erase(lru_it);
    }
  }
  fmt::print("------------------------- PC unloading {} (warm cache drop)\n", name);
  unload_level_gpu_objects(*lev, tex_pool);
  return true;
}

bool Loader::revive_from_cache(const std::string& name) {
  // Caller holds m_loader_mutex (set_want_levels). No GL calls here - the
  // textures/buffers/pool registrations were never torn down, so this is a
  // pure pointer move and the level is drawable the moment it is live again.
  auto it = m_retired_levels.find(name);
  if (it == m_retired_levels.end()) {
    return false;
  }
  it->second->frames_since_last_used = 0;
  m_retired_tex_bytes -= it->second->cached_tex_bytes;
  m_loaded_tfrag3_levels[name] = std::move(it->second);
  m_retired_levels.erase(it);
  auto lru_it = std::ranges::find(m_retired_lru, name);
  if (lru_it != m_retired_lru.end()) {
    m_retired_lru.erase(lru_it);
  }
  fmt::print("[fix58] warm cache hit: {} revived, no re-upload ({} levels remain cached)\n", name,
             m_retired_levels.size());
  return true;
}
#endif

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
    size_t live, init, want, ret;
    double ret_mb;
    {
      std::unique_lock<std::mutex> lk(m_loader_mutex);
      live = m_loaded_tfrag3_levels.size();
      init = m_initializing_tfrag3_levels.size();
      want = m_desired_levels.size();
      ret = m_retired_levels.size();
      ret_mb = (double)m_retired_tex_bytes / (1024.0 * 1024.0);
    }
    fmt::print(
        "[loader] live={} init={} want={} ret={} ({:.1f}MB tex) | pool={} bufs {:.1f}MB free, {} "
        "out | gc {} tex {} buf | budget {} (ema {:.1f}ms)\n",
        live, init, want, ret, ret_mb, m_buffer_pool.pooled_buffers(),
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
      // FIX 46 (AI-assisted): reset the age counter for a level the game STILL
      // HOLDS (m_desired_levels, i.e. __pc-set-levels), not only for the one
      // currently displayed.
      //
      // A city keeps its sub-levels resident in GOAL while only some of them
      // are being drawn. Those held-but-undisplayed levels kept aging here and
      // became the preferred eviction victims in pick_eviction_victim(), whose
      // "the game still holds this level" guard is only reached for levels with
      // a *higher* age. Then the game wanted one back immediately. That is half
      // of the evict/re-request/re-upload churn FIX 46 removes.
      const bool held = std::find(m_active_levels.begin(), m_active_levels.end(), name) !=
                            m_active_levels.end() ||
                        std::find(m_desired_levels.begin(), m_desired_levels.end(), name) !=
                            m_desired_levels.end();
      if (!held) {
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
#ifdef __SWITCH__
        // FIX 58 (AI-assisted): do not destroy what the game will most likely
        // ask for again within seconds (city shared levels get dropped on
        // every section exit). Keep the GPU objects warm instead.
        retire_to_cache(victim_name, std::move(lev), texture_pool);
#else
        fmt::print("------------------------- PC unloading {}\n", victim_name);
        unload_level_gpu_objects(*lev, texture_pool);
#endif
      }
    }
    // FIX 58b (AI-assisted): there used to be a "buffer pool under 16MB free ->
    // drop the oldest retired level" relief valve here. Hardware telemetry
    // killed it: pool free dips below 16MB (even to 0) as a NORMAL part of
    // city streaming whenever buffers are checked out, so the valve fired
    // almost every frame and the cache never held more than 1 level (ret=0 in
    // every sample, zero revivals). The caps in retire_to_cache() are the real
    // bound, and if the pool does run dry the live-level eviction above
    // retires levels into the cache, whose LRU cap then performs the actual
    // unloads - returning buffers to the pool exactly when they are needed.
    if (unload_timer.getMs() > 5.f) {
      fmt::print("Unload took {:.2f}ms\n", unload_timer.getMs());
    }
  }

#if GOAL_DEFER_MIPMAPS
  // FIX 42 (AI-assisted): pay off the deferred mip chains out of slack. While anything is
  // still streaming the frame already belongs to the loader, so only a token amount is done
  // then; once the backlog clears we catch up quickly. That is the whole point of deferring
  // them - the work happens when the player is not waiting for an area to appear.
  {
    auto evt = scoped_prof("mipmaps");
    const bool busy = loadboost_active();  // true while a blackout or a backlog is in progress
    // FIX 42a: a city load defers 600+ chains at once. Draining 2/frame would leave
    // textures unfiltered (shimmering in the distance) for ten seconds, so a large
    // backlog overrides the streaming rate -- it is still far cheaper than the 80+ ms
    // that generating them inline used to cost during the load itself.
    //
    // FIX 46b (AI-assisted): this is *in-frame* GPU work on the render thread, so it
    // has to respect the same "does the frame have room?" question as the upload
    // budget. While busy it was draining 8 chains/frame unconditionally, i.e. adding
    // roughly 8 x 0.43 ms ~= 3.4 ms to a frame that FIX 43 measured as already full.
    // When the game frame time says there is no room, drain a token amount instead
    // and let the backlog be paid off after the area has appeared -- which is exactly
    // what deferring the mips was for.
    const size_t pending_before = mipq_pending();
    int rate;
    if (!busy) {
      rate = 16;  // idle: nothing to protect, clear the backlog fast
    } else if (m_frame_gap_ema_ms > 38.0) {
      rate = 1;  // frames already being dropped: keep the in-frame cost minimal
    } else if (m_frame_gap_ema_ms > 30.0) {
      rate = 2;  // holding 30 fps: a token amount, as before
    } else {
      rate = (pending_before > 256) ? 8 : 3;  // real headroom: catch up quickly
    }

    // FIX 49 REVERTED (AI-assisted): do NOT clamp `rate` against the budget the
    // upload already spent this frame.
    //
    // That was tried on hardware on 2026-09-27 and made the city strictly worse.
    // From that build's jak2 gk_stdout.txt:
    //
    //   1214 frames chose rate=1 (84% of all drain frames)
    //   [loader] mipmaps: 768 deferred chains left   (pegged at the ceiling)
    //   stage texture took 12.13 ms                  (vs 5.2 ms in the FIX 48 build)
    //   Loader::update slow setup: 19.5ms            (vs 9.4 ms in the FIX 48 build)
    //
    // and the user reported the FIX 38 symptom returning: the dead city never
    // loaded in, and NPCs / the zoomer did not appear on re-entry.
    //
    // The reasoning was wrong in a way worth recording. The upload stage does not
    // stop at the budget - `stage texture took` is 5.2 ms against a 4-5 ms
    // catchup allowance, i.e. it is *already* over budget before this block runs.
    // So `spent_ms >= budget_ms * 0.9` was true almost always, rate collapsed to 1
    // permanently, and the consequence was not "defer the mips to later frames" as
    // intended: an ever-growing 768-deep queue of textures held at MAX_LEVEL 0
    // means hundreds of textures stay mipmap-less, each still needing MAX_LEVEL
    // raised and a glGenerateMipmap, while the queue itself holds pressure up.
    // That fed back into the upload stage (5.2 -> 12.1 ms) and into frame time,
    // and the loader then throttled itself further - the exact death spiral FIX 38
    // removed. "Backlog beats frame time" (FIX 38) is the rule; this broke it.
    //
    // The rate control above is therefore left exactly as FIX 46b set it, keyed on
    // the frame-gap EMA only, with FIX 38's guarantee that a backlog is what
    // justifies real work.

    const int did = mipq_process(rate);
    // FIX 49 (AI-assisted): diagnose-only survivor of the revert. Records what the
    // split actually was so the next hardware log can show, per frame, how the
    // budget was divided between the texture upload and the mip drain. This
    // changes no behaviour.
    g_last_mip_rate = (u32)rate;
    g_last_mip_did = (u32)did;

    static size_t s_last_bucket = (size_t)-1;
    const size_t left = mipq_pending();
    if (did > 0 && left / 256 != s_last_bucket) {
      s_last_bucket = left / 256;
      fmt::print("[loader] mipmaps: {} deferred chains left\n", left);
    }
  }
#endif

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

  // FIX 52 (AI-assisted): everything this frame's loader submission consisted of has now
  // been enqueued, so this is the one place a fence can measure the whole of it. Must be
  // the last thing update() does, and must run before the slow-setup line below so that
  // line can print the measured cost.
  //
  // Guarded because the probe's state (m_loader_gpu_ema_ms etc.) and the budget that
  // consumes it are both __SWITCH__-only -- the desktop budget is a fixed constant set in
  // LoaderStages.cpp and never retuned, so there is nothing on desktop for this to feed.
#ifdef __SWITCH__
  gpu_cost_probe();
#endif

#ifdef __SWITCH__
  if (loader_timer.getMs() > 5 || m_loader_gpu_ema_ms > 2.0) {
    // FIX 49 (AI-assisted): report the split, not just the total. The pre-FIX-49
    // log could show `slow setup: 9.1ms` without saying how much of it was the
    // mip drain that ran *after* the upload; this makes the two line items
    // greppable side by side with `stage texture took`.
    //
    // FIX 52 (AI-assisted): and report `gpu=`, the measured GPU cost of this frame's
    // loader work (see gpu_cost_probe). The trigger widened to `|| gpu > 2ms` because
    // the whole point is that these two numbers disagree: a frame whose *submit* time
    // is 1 ms can still be 15 ms of GPU work, and that frame was previously invisible.
    // A negative `gpu=` means the probe hit its 8 ms wait timeout, i.e. the real cost
    // is at least that. (AI-assisted)
    //
    // FIX 54 (AI-assisted): `gpu=` is printed as INTEGER TENTHS OF A MILLISECOND
    // (`gpu=-137` means -13.7 ms), not as a `{:.1f}` double. The first FIX 52 build
    // crashed here: `pc_off=0xaba838` symbolized to
    // fmt::v11::detail::do_write_float<..., decimal_fp<double>, ...>, i.e. the crash
    // was inside fmt's float formatting, in this very call. This line already had two
    // `{:.1f}` doubles (submit time and budget) before FIX 52 added a third; that is
    // three doubles through fmt's variadic path on this target, and it does not
    // survive. Integer tenths keep the same 0.1 ms resolution, stay readable, and take
    // the third double back out of the argument list. Do not reintroduce a float here.
    fmt::print(
        "Loader::update slow setup: {:.1f}ms (mip rate={} did={}, budget={:.1f}ms, gpu={})\n",
        loader_timer.getMs(), g_last_mip_rate, g_last_mip_did, (double)g_loader_budget.ms,
        (int)(m_loader_gpu_last_ms * 10.0));
  }
#endif
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
#ifdef __SWITCH__
  // FIX 58: a forced reload wants fresh data - a warm-cached copy of this
  // level must not be revived instead of reloading it.
  drop_retired_level(name, texture_pool);
#endif
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

#ifdef __SWITCH__
  // FIX 58: a full reload is a clean slate - the warm cache goes too,
  // otherwise stale copies would be revived over the freshly reloaded data.
  while (drop_oldest_retired(texture_pool)) {
  }
#endif

  for (auto buf : m_garbage_buffers)
    glDeleteBuffers(1, &buf);
  m_garbage_buffers.clear();
  flush_texture_garbage();
  // A full reload wants a clean slate: actually delete the pooled buffers
  // instead of keeping them around for reuse (FIX 33).
  m_buffer_pool.clear();

  set_want_levels(m_desired_levels);
}