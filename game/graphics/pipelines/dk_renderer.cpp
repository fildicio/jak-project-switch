/*!
 * @file dk_renderer.cpp
 * deko3d migration Phase 1 (AI-assisted): the GfxRendererModule for the native deko3d
 * backend. See dk_renderer.h for scope. libnx/deko3d calls live in dk_device.cpp; this
 * TU uses the project's normal headers (SDL compat for input, run_log for telemetry).
 *
 * Engine handshake: identical contract to the GL module (gl_vsync / gl_sync_path /
 * gl_send_chain, incl. the FIX 11 single-mutex rule and the FIX 19 "frame_idx must
 * advance every iteration" rule), so the GOAL engine runs unchanged. Received DMA chains
 * are acknowledged but not rendered in Phase 1 -- the screen shows the boot splash, then
 * black. Level/texture entry points are accepted and ignored for now (P2/P3).
 */

#include "game/graphics/pipelines/dk_renderer.h"

#if defined(__SWITCH__)

#include <algorithm>
#include <atomic>  // T49: s_dk_loader_ptr
#include <chrono>
#include <cstdio>
#include <cstring>  // T24: strlen() while assembling the soak telemetry line
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>  // std::this_thread::sleep_for when out of focus (T10)
#include <unistd.h>

#include "common/goal_constants.h"  // T49: jakN::LEVEL_TOTAL for the dk-path Loader
#include "common/log/log.h"
#include "common/util/FileUtil.h"     // T49: file_util::get_jak_project_dir
#include "common/util/Timer.h"

#include "game/graphics/display.h"
#include "game/graphics/pipelines/dk_device.h"
#include "game/graphics/deko3d_renderer/dk_texture_gpu.h"  // T24: P2.3 dktexgpu telemetry
#include "game/graphics/deko3d_renderer/TFragmentDk.h"     // T60: tfrag world pass
#include "game/graphics/deko3d_renderer/dk_gl_census.h"   // T49: no-op glad stubs + census
#include "game/graphics/opengl_renderer/loader/Loader.h"   // T49: opt-in dk-path Loader
#include "game/graphics/texture/TexturePool.h"             // T49
#include "game/kernel/common/kboot.h"
#include "game/runtime.h"
#include "game/switch/platform.h"
#include "game/switch/run_log.h"
#include "game/switch/sdl3_compat.h"
#include "game/system/hid/display_manager.h"
#include "game/system/hid/input_manager.h"

namespace {

struct DkSyncData {
  std::mutex sync_mutex;
  std::condition_variable sync_cv;
  std::mutex dma_mutex;
  std::condition_variable dma_cv;
  u64 frame_idx = 0;
  u64 frame_idx_of_input_data = 0;
  bool has_data_to_render = false;
  u64 chains_received = 0;
  // T60: the last chain from dk_send_chain. Only valid until has_data_to_render is cleared.
  const void* chain_data = nullptr;
  u32 chain_offset = 0;
};
std::unique_ptr<DkSyncData> g_dk;

int dk_init(GfxGlobalSettings& /*settings*/) {
  // Gamepads only. SDL's Switch video driver would claim the default native window for
  // EGL, which deko3d needs -- so SDL_INIT_VIDEO is never called on this path (plan P1.2).
  SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
  if (!SDL_Init(SDL_INIT_EVENTS)) {
    switch_run_logf("[dk] SDL_Init(EVENTS) failed: %s", SDL_GetError());
  }
  g_dk = std::make_unique<DkSyncData>();
  return 0;
}

// T49 (AI-assisted, P2.3 bring-up): OPT-IN dk-path Loader, only when
// sdmc:/switch/<game>/gk_dkload.txt exists (on top of gk_dk3d.txt). The Loader still
// runs its GL staging code; dk_gl_census stubs every glad pointer (no context here) so
// those calls are no-ops, and the run log names each GL function the Loader reaches —
// the exact list of seams still to write. Level textures go to dktexgpu via the T43
// seam. Created/updated on the render thread only (T47 owner-thread rule).
constexpr int kDkFr3LevelCount[] = {jak1::LEVEL_TOTAL, jak2::LEVEL_TOTAL, jak3::LEVEL_TOTAL,
                                    jakx::LEVEL_TOTAL};
std::unique_ptr<TexturePool> s_dk_tex_pool;
std::unique_ptr<Loader> s_dk_loader;
std::atomic<Loader*> s_dk_loader_ptr{nullptr};  // read by the EE thread (set_levels)

bool dkload_present() {
  char path[96];
  snprintf(path, sizeof(path), "sdmc:/switch/%s/gk_dkload.txt",
           game_version_names[g_game_version]);
  return access(path, F_OK) == 0;
}

// T60 (AI-assisted): OPT-IN P3 tfrag world pass, only with sdmc:/switch/jak1/gk_dktfrag.txt
// on top of gk_dk3d.txt + gk_dkload.txt. jak1 only (bucket ids are jak1's).
bool s_dk_tfrag = false;

bool dktfrag_present() {
  char path[96];
  snprintf(path, sizeof(path), "sdmc:/switch/%s/gk_dktfrag.txt",
           game_version_names[g_game_version]);
  return access(path, F_OK) == 0;
}

void dk_loader_start() {
  dkglcensus::install();
  // T51 breadcrumbs: the T49 run froze silently inside this function.
  switch_run_logf("[dk] dk-loader step 1/3: TexturePool (placeholder upload)");
  s_dk_tex_pool = std::make_unique<TexturePool>(g_game_version);
  const auto fr3 =
      file_util::get_jak_project_dir() / "out" / game_version_names[g_game_version] / "fr3";
  switch_run_logf("[dk] dk-loader step 2/3: Loader(%s)", fr3.string().c_str());
  s_dk_loader = std::make_unique<Loader>(fr3, kDkFr3LevelCount[(int)g_game_version],
                                         g_game_version);
  switch_run_logf("[dk] dk-loader step 3/3: load_common GAME.fr3");
  s_dk_loader->load_common(*s_dk_tex_pool, "GAME");
  s_dk_loader_ptr.store(s_dk_loader.get());
  switch_run_logf("[dk] dk-loader up (gk_dkload.txt): common loaded, tex live=%u fail=%d",
                  dktexgpu::live_count(), dktexgpu::failed_allocs());
  if (g_game_version == GameVersion::Jak1 && dktfrag_present()) {
    s_dk_tfrag = true;
    dkdev::set_world_recorder(dktf::record_world);
    switch_run_logf("[dk] tfrag world pass ON (gk_dktfrag.txt)");
    // R1 (AI-assisted): one-shot replay dump for the host test. The file may hold the
    // number of tfrag frames to wait (default 300 = ~10 s at 30 fps).
    char path[96];
    snprintf(path, sizeof(path), "sdmc:/switch/%s/gk_dkdump.txt",
             game_version_names[g_game_version]);
    if (FILE* f = fopen(path, "r")) {
      int frames = 300;
      if (fscanf(f, "%d", &frames) != 1 || frames < 0) {
        frames = 300;
      }
      fclose(f);
      char dir[64];
      snprintf(dir, sizeof(dir), "sdmc:/switch/%s", game_version_names[g_game_version]);
      dktf::arm_replay_dump(dir, frames);
    }
  }
}

void dk_exit() {
  // T60: stop recording the world before the Loader (and its level data) goes away.
  dkdev::set_world_recorder(nullptr);
  s_dk_tfrag = false;
  // T49: stop the opt-in Loader first (joins its file thread) while the device is up.
  s_dk_loader_ptr.store(nullptr);
  s_dk_loader.reset();
  s_dk_tex_pool.reset();
  // T48 (AI-assisted): free the texture image pool BEFORE the device goes away; drain the
  // queue first so no in-flight upload still reads the memblock being destroyed.
  dkdev::wait_idle();
  dktexgpu::teardown();
  dkdev::shutdown();
  g_dk.reset();
}

class DkDisplay : public GfxDisplay {
 public:
  explicit DkDisplay(bool is_main)
      : m_display_manager(std::make_shared<DisplayManager>(nullptr)),
        m_input_manager(std::make_shared<InputManager>(nullptr)) {
    m_main = is_main;
    m_display_manager->set_input_manager(m_input_manager);
  }
  ~DkDisplay() override {
    m_display_manager.reset();
    m_input_manager.reset();
    SDL_Quit();
    if (m_main) {
      dk_exit();
    }
  }

  std::shared_ptr<DisplayManager> get_display_manager() const override {
    return m_display_manager;
  }
  std::shared_ptr<InputManager> get_input_manager() const override { return m_input_manager; }

  void render() override;
  void init_splash() override;
  void draw_splash(int, int) override {}

 private:
  std::shared_ptr<DisplayManager> m_display_manager;
  std::shared_ptr<InputManager> m_input_manager;
  bool m_should_quit = false;
  bool m_splash_done = false;
  unsigned m_swap_interval = ~0u;  // force the first frame to apply it
  int m_op_mode = -1;             // docked/handheld state for P1.2b change logging (T09)
  bool m_focused = true;          // pause present while out of focus (T10 / P1.2b)

  struct Stats {
    int frames = 0, chains = 0;
    double wait = 0, record = 0, submit = 0, total = 0, max_total = 0;
  } m_st;
  Timer m_report;
  Timer m_period;
  bool m_first = true;

  void process_events();
};

void DkDisplay::init_splash() {
  if (m_splash_done || !Gfx::g_splash.ready.load() || Gfx::g_splash.data.empty()) {
    return;
  }
  m_splash_done = true;
  dkdev::upload_splash(Gfx::g_splash.data.data(), Gfx::g_splash.width, Gfx::g_splash.height);
  Gfx::g_splash.data.clear();
  Gfx::g_splash.data.shrink_to_fit();
}

void DkDisplay::process_events() {
  if (!switch_platform::applet_pump()) {  // FIX 7p: acknowledge applet messages every frame
    m_should_quit = true;
  }
  SDL_Event evt;
  while (SDL_PollEvent(&evt) != 0) {
    if (evt.type == SDL_EVENT_QUIT) {
      switch_run_logf("[dk] SDL_EVENT_QUIT received");
      m_should_quit = true;
    }
    m_display_manager->process_sdl_event(evt);
    m_input_manager->process_sdl_event(evt);
  }
  const int mode = dkdev::operation_mode();
  if (mode != m_op_mode) {
    switch_run_logf("[dk] operation mode: %s (swapchain stays at boot size until P1.2b resize)",
                    mode ? "docked" : "handheld");
    m_op_mode = mode;
  }
}

void DkDisplay::render() {
  if (m_first) {
    m_first = false;
    m_report.start();
    m_period.start();
    switch_run_logf("[dk] first render; gamepads seen by SDL: %d (no SDL video on this path)",
                    m_input_manager->get_num_controllers());
    // T48 (AI-assisted, P2.3): bring up the 48 MB texture image pool HERE, on the render
    // thread, so the T47 owner-thread guard records this thread as the only uploader.
    const bool tex_ok = dktexgpu::init();
    switch_run_logf("[dk] tex pool init: %s",
                    tex_ok ? "ok (48 MB image memblock, render thread owns uploads)"
                           : "FAILED (telemetry stays 'tex -')");
    if (tex_ok && dkload_present()) {
      dk_loader_start();  // T49
    }
  }
  m_input_manager->poll_keyboard_data();
  m_input_manager->poll_mouse_data();
  m_input_manager->finish_polling();
  process_events();
  m_display_manager->process_ee_events();
  m_input_manager->process_ee_events();
  m_display_manager->set_game_size(Gfx::g_global_settings.lbox_w, Gfx::g_global_settings.lbox_h);

  if (SplashScreen && DiskBoot && !m_splash_done) {
    init_splash();
  }
  const bool show_splash =
      SplashScreen && m_splash_done && SplashTimer.getSeconds() < SPLASH_SCREEN_TIME;

  // Engine handshake, same contract as render_game_frame() on the GL path: wait up to
  // 40 ms for a chain, then mark it consumed under dma_mutex (FIX 11). Phase 1 does not
  // draw it yet.
  bool got_chain = false;
  {
    std::unique_lock<std::mutex> lock(g_dk->dma_mutex);
    got_chain = g_dk->dma_cv.wait_for(lock, std::chrono::milliseconds(40),
                                      [] { return g_dk->has_data_to_render; });
  }
  if (got_chain) {
    {
      std::unique_lock<std::mutex> lock(g_dk->sync_mutex);
      g_dk->frame_idx_of_input_data = g_dk->frame_idx;
    }
    std::unique_lock<std::mutex> lock(g_dk->dma_mutex);
    if (s_dk_tfrag) {
      // T60: read the chain BEFORE the ack below -- after it the EE rewrites that memory.
      dktf::capture_chain(g_dk->chain_data, g_dk->chain_offset);
    }
    g_dk->has_data_to_render = false;
    g_dk->dma_cv.notify_all();
  }

  // swap interval from the target framerate (FIX 10 semantics: 30 fps = interval 2)
  unsigned want = Gfx::g_global_settings.vsync ? 1 : 0;
  if (Gfx::g_global_settings.vsync && Gfx::g_global_settings.target_fps > 0 &&
      Gfx::g_global_settings.target_fps <= 35) {
    want = 2;
  }
  if (want != m_swap_interval) {
    m_swap_interval = want;
    dkdev::set_swap_interval(want);
    switch_run_logf("[dk] swap interval %u (vsync=%d target_fps=%.0f)", want,
                    (int)Gfx::g_global_settings.vsync, Gfx::g_global_settings.target_fps);
  }

  // T10 / P1.2b: stop presenting while out of focus (HOME menu / overlay / sleep). The
  // present_frame() call is the only GPU-submitting step in this function; guarding it is
  // enough to pause GPU work. The engine handshake below (frame_idx advance + sync_cv
  // notify + the chain-acknowledge above) is NOT gated on focus: per FIX 19 the engine
  // relies on frame_idx advancing every iteration and on sync_cv being notified, so the
  // render loop must never deadlock on an unfocused frame.
  const bool focused = dkdev::in_focus();
  if (focused != m_focused) {
    switch_run_logf("[dk] focus %s", focused ? "regained" : "lost: pausing present");
    if (!focused) {
      // Drain once so nothing is in flight while suspended (plan P1.2b focus/suspend).
      dkdev::wait_idle();
    }
    m_focused = focused;
  }
  if (s_dk_loader) {
    s_dk_loader->update(*s_dk_tex_pool);  // T49: render thread, before present
  }
  if (s_dk_tfrag && got_chain && focused) {
    // T60: before present_frame -- palette uploads reuse the frame cmd buffer. Only when
    // a present follows (the index ring relies on one present per build).
    dktf::build_frame(s_dk_loader.get());
  }
  dkdev::FrameTiming ft{};
  if (focused) {
    ft = dkdev::present_frame(show_splash);
  } else {
    // Out of focus: do not submit, but keep the ~60 Hz pacing so the loop doesn't spin.
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }

  // FIX 19: frame_idx must advance every iteration -- it is the engine's liveness signal.
  {
    std::unique_lock<std::mutex> lock(g_dk->sync_mutex);
    g_dk->frame_idx++;
    g_dk->sync_cv.notify_all();
  }

  // [dk] telemetry (plan P1.5): every 2 s with R3+Minus diag on, else every 10 s, since
  // each SD line costs ~6 ms (FIX 40). A non-presenting (out-of-focus) frame contributes
  // no timing samples, so skip accumulation when unfocused; the loop still paces at
  // ~16 ms (see the sleep above).
  const double total = m_period.getMs();
  m_period.start();
  if (focused) {
    m_st.frames++;
    m_st.chains += got_chain ? 1 : 0;
    m_st.wait += ft.wait_ms;
    m_st.record += ft.record_ms;
    m_st.submit += ft.submit_ms;
    m_st.total += total;
    m_st.max_total = std::max(m_st.max_total, total);
  }
  const double window = switch_diag_enabled() ? 2.0 : 10.0;
  if (m_report.getSeconds() >= window && m_st.frames > 0) {
    const double n = (double)m_st.frames;
    const dkdev::HeapInfo h = dkdev::heap_info();
    switch_run_logf("[dk] %.1f fps | frame %.2f (max %.1f) ms | wait %.2f record %.3f submit %.3f "
                    "| chains %d/%d | pools %llu KB | heap free %llu MB",
                    n / m_report.getSeconds(), m_st.total / n, m_st.max_total, m_st.wait / n,
                    m_st.record / n, m_st.submit / n, m_st.chains, m_st.frames,
                    dkdev::pool_bytes() / 1024, h.total_free() >> 20);
    // T24 (P2.3 soak telemetry): one more [dk] line in the SAME report window as the
    // fps line just above (piggyback its exact timing/guard). Null-check each
    // subsystem so an inactive group prints "<name> -" instead of numbers; never
    // crash, never spam per frame (shares the report-window guard, so at most
    // every 2 s diag-on / 10 s diag-off, like the fps line).
    {
      const dkring::FrameRing* ring = dkdev::ubo_ring();
      const dkpool::BufferPool* pool = dkdev::buffer_pool();
      char tel[192];
      // tex: dktexgpu (T22) telemetry; active() = the 48 MB image memblock exists.
      if (dktexgpu::active()) {
        snprintf(tel, sizeof(tel), "[dk] tex live=%u high=%u fail=%d | ",
                 dktexgpu::live_count(), dktexgpu::high_water(), dktexgpu::failed_allocs());
      } else {
        snprintf(tel, sizeof(tel), "[dk] tex - | ");
      }
      // pool: P2.3 BufferPool (T21); null when dk_device never carved its region.
      size_t pos = strlen(tel);
      if (pool != nullptr) {
        snprintf(tel + pos, sizeof(tel) - pos, "pool out=%u pooled=%u fail=%d | ",
                 pool->outstanding(), pool->pooled_chunks(), pool->failed_allocations());
      } else {
        snprintf(tel + pos, sizeof(tel) - pos, "pool - | ");
      }
      // ring: UBO ring (T20): used / segment_size / high_water; null when not wired.
      pos = strlen(tel);
      if (ring != nullptr) {
        snprintf(tel + pos, sizeof(tel) - pos, "ring used=%u/%u hw=%u", ring->used(),
                 ring->segment_size(), ring->high_water());
      } else {
        snprintf(tel + pos, sizeof(tel) - pos, "ring -");
      }
      switch_run_logf("%s", tel);
    }
    m_st = Stats();
    m_report.start();
  }

  if (m_should_quit) {
    switch_run_logf("[dk] quit latched -> MasterExit=EXIT");
    {
      std::unique_lock<std::mutex> lock(g_dk->sync_mutex);
      MasterExit = RuntimeExitStatus::EXIT;
      g_dk->sync_cv.notify_all();
    }
    std::unique_lock<std::mutex> lock(g_dk->dma_mutex);
    g_dk->dma_cv.notify_all();
  }
}

std::shared_ptr<GfxDisplay> dk_make_display(int /*width*/,
                                            int /*height*/,
                                            const char* /*title*/,
                                            GfxGlobalSettings& /*settings*/,
                                            GameVersion /*version*/,
                                            bool is_main) {
  // Same sizing rule as the GL path (FIX 73c): decided once, from the operation mode.
  const auto size = switch_platform::get_display_size_for_operation_mode();
  if (!dkdev::init(size.w, size.h)) {
    return nullptr;  // Gfx::Init falls back to OpenGL
  }
  switch_platform::set_created_window_size(size.w, size.h);
  auto display = std::make_shared<DkDisplay>(is_main);
  display->get_display_manager()->set_native_window(size.w, size.h);
  display->set_imgui_visible(Gfx::g_debug_settings.show_imgui);
  return display;
}

u32 dk_vsync() {
  if (!g_dk) {
    return 0;
  }
  std::unique_lock<std::mutex> lock(g_dk->sync_mutex);
  const auto init_frame = g_dk->frame_idx_of_input_data;
  g_dk->sync_cv.wait(lock, [=] {
    return (MasterExit != RuntimeExitStatus::RUNNING) || g_dk->frame_idx > init_frame;
  });
  return g_dk->frame_idx & 1;
}

u32 dk_sync_path() {
  if (!g_dk) {
    return 0;
  }
  // FIX 11: wait on dma_cv under dma_mutex (the predicate's own mutex).
  std::unique_lock<std::mutex> lock(g_dk->dma_mutex);
  if (!g_dk->has_data_to_render) {
    return 0;
  }
  g_dk->dma_cv.wait(lock, [] {
    return (MasterExit != RuntimeExitStatus::RUNNING) || !g_dk->has_data_to_render;
  });
  return 0;
}

void dk_send_chain(const void* data, u32 offset) {
  if (!g_dk) {
    return;
  }
  std::unique_lock<std::mutex> lock(g_dk->dma_mutex);
  if (g_dk->has_data_to_render) {
    lg::error("Gfx::send_chain called when the deko3d renderer has pending data");
    return;
  }
  // Phase 1: chains are acknowledged, not rendered. T60: the pointer is kept for the
  // opt-in tfrag capture, which reads it on the render thread before the ack.
  g_dk->chain_data = data;
  g_dk->chain_offset = offset;
  g_dk->has_data_to_render = true;
  g_dk->chains_received++;
  g_dk->dma_cv.notify_all();
}

// P2/P3 entry points: accepted and ignored for now.
void dk_texture_upload_now(const u8*, int, u32) {}
void dk_texture_relocate(u32, u32, u32) {}
void dk_set_levels(const std::vector<std::string>& levels) {
  if (Loader* l = s_dk_loader_ptr.load()) {
    l->set_want_levels(levels);  // T49: same call as gl_set_levels
  }
}
void dk_set_active_levels(const std::vector<std::string>& levels) {
  if (Loader* l = s_dk_loader_ptr.load()) {
    l->set_active_levels(levels);  // T49: same call as gl_set_active_levels
  }
}
void dk_force_reload_all() {}
void dk_force_reload_level(const std::string&) {}
void dk_force_reload_common() {}
void dk_set_pmode_alp(float) {}

}  // namespace

namespace dk_renderer {
bool kill_switch_present(GameVersion version) {
  char path[96];
  snprintf(path, sizeof(path), "sdmc:/switch/%s/gk_dk3d.txt", game_version_names[version]);
  return access(path, F_OK) == 0;
}
}  // namespace dk_renderer

const GfxRendererModule gRendererDeko3D = {
    dk_init,                 // init
    dk_make_display,         // make_display
    dk_exit,                 // exit
    dk_vsync,                // vsync
    dk_sync_path,            // sync_path
    dk_send_chain,           // send_chain
    dk_texture_upload_now,   // texture_upload_now
    dk_texture_relocate,     // texture_relocate
    dk_set_levels,           // set_levels
    dk_set_active_levels,    // set_active_levels
    dk_force_reload_all,     // force_reload_all
    dk_force_reload_level,   // force_reload_level
    dk_force_reload_common,  // force_reload_common
    dk_set_pmode_alp,        // set_pmode_alp
    GfxPipeline::Deko3D,     // pipeline
    "deko3d (P1 skeleton)"   // name
};

#endif  // __SWITCH__
