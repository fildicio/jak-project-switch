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
#include "game/graphics/deko3d_renderer/HfragDk.h"          // T113: hfrag (mountains)
#include "game/graphics/deko3d_renderer/ShrubDk.h"         // T72: shrub world pass
#include "game/graphics/deko3d_renderer/TieDk.h"           // T76: tie world pass
#include "game/graphics/deko3d_renderer/MercDk.h"          // T78: merc2 pass
#include "game/graphics/deko3d_renderer/SpriteDk.h"        // T79: sprite3 pass
#include "game/graphics/deko3d_renderer/DirectDk.h"        // T80: menus / HUD text
#include "game/graphics/deko3d_renderer/SkyDk.h"           // T88: jak1 sky CPU blend
#include "game/graphics/deko3d_renderer/dk_ocean_driver.h"  // T95: jak1 ocean
#include "game/graphics/deko3d_renderer/dkgl/dkgl_frame.h"  // T107: GL ocean through dkgl
#include "game/graphics/deko3d_renderer/EyeDk.h"           // T82: render-to-texture eyes
#include "game/graphics/deko3d_renderer/dk_gl_census.h"   // T49: no-op glad stubs + census
#include "game/graphics/opengl_renderer/loader/Loader.h"   // T49: opt-in dk-path Loader
#include "game/graphics/texture/TexturePool.h"             // T49
#include "game/kernel/common/kboot.h"
#include "game/kernel/common/kmachine.h"  // T79: offset_of_s7 for texture uploads
#include "game/runtime.h"
#include "game/switch/platform.h"
#include "game/switch/run_log.h"
#include "game/switch/sdl3_compat.h"
#include "game/system/hid/display_manager.h"
#include "game/system/hid/input_manager.h"

namespace {

// T83 (AI-assisted): frame-time split, render thread only. Hardware tick counter (same
// clock as dk_device present_frame); no hooks on the EE (GOAL) side.
inline u64 dk_tick() {
  return dkdev::tick_now();
}
inline double dk_ms(u64 a, u64 b) {
  return dkdev::tick_ms(a, b);
}

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
std::atomic<float> s_dk_pmode_alp{1.f};  // GOAL set_pmode_alp (EE thread), read per frame

// TODO #12 (AI-assisted): jak1 runs on deko3d by default, so every opt-in pass file
// counts as present there; the one opt-out is gk_opengl.txt (checked in
// kill_switch_present). jak2/jak3 keep the per-pass opt-in files until they render right.
bool dk_pass_on(GameVersion version, const char* file) {
  if (version == GameVersion::Jak1) {
    return true;
  }
  char path[96];
  snprintf(path, sizeof(path), "sdmc:/switch/%s/%s", game_version_names[version], file);
  return access(path, F_OK) == 0;
}

bool dkload_present() {
  return dk_pass_on(g_game_version, "gk_dkload.txt");
}

// T60 (AI-assisted): OPT-IN P3 tfrag world pass, only with sdmc:/switch/jak1/gk_dktfrag.txt
// on top of gk_dk3d.txt + gk_dkload.txt. jak1 only (bucket ids are jak1's).
bool s_dk_tfrag = false;
// T72 (AI-assisted): OPT-IN shrub pass, gk_dkshrub.txt on top of gk_dktfrag.txt (jak1 only).
bool s_dk_shrub = false;
// T76 (AI-assisted): OPT-IN tie pass, gk_dktie.txt on top of gk_dktfrag.txt (jak1 only).
bool s_dk_tie = false;
// T113/FIX 117 (AI-assisted): the GL Hfrag renderer is a JAK3 renderer (jak3's HFRAG
// bucket is 8). Jak1's bucket 8 is TIE_NEAR_LEVEL0, which the game leaves empty (GL
// registers nothing there either) and no jak1 fr3 level carries hfrag data -- the T113
// port could never draw on jak1. Off until the jak3 port flips this in its own init.
bool s_dk_hfrag = false;
// T78 (AI-assisted): merc2 (characters) rides on the tie flag (no extra SD file);
// gk_nomerc.txt turns it off again.
bool s_dk_merc = false;
// T79 (AI-assisted): sprite3 also rides on the tie flag; gk_nosprite.txt turns it off.
bool s_dk_sprite = false;
// T80 (AI-assisted): direct buckets (menus, HUD text, subtitles) ride on the sprite flag;
// gk_nodirect.txt turns them off.
bool s_dk_direct = false;
// T88 (AI-assisted): OPT-IN jak1 sky pass (GL SkyRenderer bucket 3 + SkyBlendHandler
// buckets 32/39) riding on the direct flag; gk_nosky.txt turns it off again.
bool s_dk_sky = false;
// T95 (AI-assisted): OPT-IN jak1 ocean pass (GL ocean-mid-far bucket 4 +
// ocean-near bucket 63) on top of tfrag; gk_noocean.txt turns it off again.
bool s_dk_ocean = false;
void dk_drain_tex_ops();  // T80: queued GOAL upload-now / relocate, defined below

// T79: TextureUploadHandler::flush_uploads on the dk pool (render thread, chain time).
const u8* s_dk_upload_base = nullptr;
void dk_sprite_upload(uint32_t page, int64_t mode) {
  if (s_dk_tex_pool && s_dk_upload_base && page < (uint32_t)EE_MAIN_MEM_SIZE) {
    s_dk_tex_pool->handle_upload_now(s_dk_upload_base + page, (int)mode, s_dk_upload_base,
                                     offset_of_s7(), false);
  }
}
// T79: TexturePool::lookup -> dktexgpu id; 0 when empty or still a placeholder.
uint32_t dk_sprite_lookup(uint32_t tbp) {
  if (!s_dk_tex_pool) {
    return 0;
  }
  GpuTexture* src = s_dk_tex_pool->lookup_gpu_texture(tbp);
  if (src == nullptr || src->is_placeholder) {
    return 0;
  }
  auto id = s_dk_tex_pool->lookup(tbp);
  return id ? (uint32_t)*id : 0;
}

// T80: GL DirectRenderer::update_gl_texture lookup (mt4hh for the font); 0 = missing.
uint32_t dk_direct_lookup(uint32_t tbp, bool mt4hh) {
  if (!s_dk_tex_pool) {
    return 0;
  }
  std::optional<u64> id;
  if (mt4hh) {
    id = s_dk_tex_pool->lookup_mt4hh(tbp);
  } else {
    GpuTexture* src = s_dk_tex_pool->lookup_gpu_texture(tbp);
    if (src == nullptr || src->is_placeholder) {
      return 0;
    }
    id = s_dk_tex_pool->lookup(tbp);
  }
  return id ? (uint32_t)*id : 0;
}

// T88 (AI-assisted): GL SkyBlendCPU::init_textures for the dk path. The two blended
// outputs (32x32 sky, 64x64 clouds) are dktexgpu textures registered in the pool at the
// fixed VRAM slots the sky-draw bucket (3) samples, SKY_TEXTURE_VRAM_ADDRS; the pool's
// `gpu_texture` field holds the dktexgpu id, exactly like every other dk texture.
// Allocated once and never released (P8: image memory is never reclaimed anyway).
GpuTexture* s_sky_gpu[2] = {nullptr, nullptr};
uint32_t s_sky_tex_id[2] = {0, 0};
uint32_t s_sky_store_fails = 0;

void dk_sky_init_textures() {
  static const uint8_t zeros[4 * 64 * 64] = {0};
  if (!s_dk_tex_pool) {
    return;
  }
  for (int i = 0; i < 2; i++) {
    const int size = (i == 0) ? 32 : 64;  // GL SkyBlendCPU m_sizes
    s_sky_tex_id[i] = dktexgpu::upload_rgba8(size, size, zeros);
    if (s_sky_tex_id[i] == 0) {
      switch_run_logf("[dk] sky tex %d alloc failed (store full / memblock)", i);
      continue;
    }
    TextureInput in;
    in.gpu_texture = s_sky_tex_id[i];
    in.w = size;
    in.h = size;
    in.debug_name = (i == 0) ? "PC-SKY-CPU-0" : "PC-SKY-CPU-1";
    in.id = s_dk_tex_pool->allocate_pc_port_texture(g_game_version);
    s_sky_gpu[i] = s_dk_tex_pool->give_texture_and_load_to_vram(in, SKY_TEXTURE_VRAM_ADDRS[i]);
  }
  switch_run_logf("[dk] sky textures at tbp %d/%d: ids %u %u", SKY_TEXTURE_VRAM_ADDRS[0],
                  SKY_TEXTURE_VRAM_ADDRS[1], s_sky_tex_id[0], s_sky_tex_id[1]);
}

// T88: SourceFn -- GL do_sky_blends' pool lookup. RGBA level textures keep a CPU pixel
// pointer in the pool (LoaderStages sets TextureInput::src_data for exactly this reader);
// BCn entries have none and count as no_src in the blend stats.
dksky::SkySource dk_sky_source(uint32_t tbp) {
  dksky::SkySource out;
  if (!s_dk_tex_pool) {
    return out;
  }
  GpuTexture* tex = s_dk_tex_pool->lookup_gpu_texture(tbp);
  if (tex == nullptr) {
    return out;
  }
  if (tex->is_placeholder) {
    // FIX 117: distinguish "not uploaded yet" from "placeholder" in the sky stats.
    out.placeholder = true;
    return out;
  }
  out.data = tex->get_data_ptr();
  out.bytes = tex->data_size();
  return out;
}

// T88: SinkFn -- push one blended buffer to its texture. Build time, right after
// begin_deferred_frame(): the copy is staged now and recorded by
// record_deferred_updates() at the top of the world recorder, before the sky draws
// sample the texture (same T86 path as the tod palettes).
void dk_sky_store(int index, const uint8_t* rgba, uint32_t /*size*/) {
  if (index < 0 || index > 1 || s_sky_tex_id[index] == 0) {
    return;
  }
  const int size = (index == 0) ? 32 : 64;
  if (!dktexgpu::update_rgba8_deferred(s_sky_tex_id[index], size, size, rgba)) {
    s_sky_store_fails++;
  }
  // FIX 112: GL SkyBlendCPU re-pins after every upload; level loads can take the slot.
  if (s_dk_tex_pool && s_sky_gpu[index]) {
    s_dk_tex_pool->move_existing_to_vram(s_sky_gpu[index], SKY_TEXTURE_VRAM_ADDRS[index]);
  }
}

// FIX 112: dksprite::BucketHookFn. Called before each replayed texture-upload bucket; runs
// a level's sky blend once the replay has passed its bucket (32 / 39), i.e. with the VRAM
// state GL's SkyBlendHandler sees. Later uploads (pris, water...) reuse those addresses.
const void* s_sky_hook_data = nullptr;
uint32_t s_sky_hook_offset = 0;
int s_sky_hook_next = 0;  // next sky level to blend this frame (0, 1, 2 = done)
void dk_sky_bucket_hook(int bucket) {
  while (s_sky_hook_data && s_sky_hook_next < 2 &&
         bucket > dksky::kBlendBucket[s_sky_hook_next]) {
    dksky::capture_blend_level(s_sky_hook_data, s_sky_hook_offset, dk_sky_source,
                               s_sky_hook_next);
    s_sky_hook_next++;
  }
}

// T84: pass order for the per-pass timing (GPU marks 1..7, CPU build slots).
enum DkPass { kPTfrag, kPTie, kPShrub, kPEye, kPMerc, kPSprite, kPDirect, kPassCount };
const char* const kPassName[kPassCount] = {"tfrag", "tie", "shrub", "eye", "merc", "sprite",
                                           "direct"};

void record_tfrag_and_shrub(int fb_w, int fb_h) {
  dktexgpu::record_deferred_updates();  // T86: tod palette copies + T88 sky uploads first
  if (s_dk_sky) {
    dkdirect::record_sky_world(fb_w, fb_h);  // T88: GL bucket order -- sky 3 before tfrag 6
  }
  if (s_dk_ocean) {
    // T95: GL bucket order -- ocean-mid-far 4 after sky 3, before tfrag 6
    // (texture gen -> far draws -> mid body flush).
    if (dkgl::active()) {
      dkgl::set_frame_size(fb_w, fb_h);
      // T107: the GL ocean, recorded at capture time. The far ocean (GL DirectRenderer,
      // direct_basic_textured) runs through dkgl too (AI-assisted).
      dkgl::record_bucket(4);
    } else {
      dkocean::record_mid_far(fb_w, fb_h);
    }
  }
  dktf::record_world(fb_w, fb_h);
  dkdev::gpu_mark(1 + kPTfrag);
  if (s_dk_hfrag) {
    dkhfrag::record_world(fb_w, fb_h);  // T113: GL bucket order -- hfrag 8 after tfrag 6/7
  }
  if (s_dk_tie) {
    dktie::record_world(fb_w, fb_h);  // GL bucket order: tfrag 6, tie 9, shrub 20
    dkdev::gpu_mark(1 + kPTie);
  }
  if (s_dk_shrub) {
    dkshrub::record_world(fb_w, fb_h);
    dkdev::gpu_mark(1 + kPShrub);
  }
  if (dkgl::active()) {
    // T111: Generic2 tfrag/shrub buckets (11 18 24 30) after the opaque native world
    for (int b : {11, 18, 24, 30}) {
      dkgl::record_bucket(b);
    }
  }
  if (s_dk_merc && s_dk_direct) {
    dkeye::record_eyes();  // T82: eye textures first, merc samples them (rebinds the frame target)
    dkdev::gpu_mark(1 + kPEye);
  }
  if (s_dk_merc) {
    dkmerc::record_world(fb_w, fb_h);  // GL: merc buckets 10+ come after the opaque world
    dkdev::gpu_mark(1 + kPMerc);
  }
  if (dkgl::active()) {
    // T111: Generic2 alpha / pris / water buckets (46 50 53 56 59 62), before ocean-near 63;
    // T113: the shadow (47) between common-alpha 46 and l0-pris 50, after merc, as in GL
    for (int b : {46, 47, 50, 53, 56, 59, 62}) {
      dkgl::record_bucket(b);
    }
  }
  if (s_dk_ocean) {
    // T95: GL bucket order -- ocean-near 63 after the water buckets, before
    // depth-cue/sprite 66.
    if (dkgl::active()) {
      dkgl::record_bucket(63);  // T107
    } else {
      dkocean::record_near(fb_w, fb_h);
    }
  }
  if (dkgl::active()) {
    dkgl::record_bucket(64);  // T112: depth-cue blurs the finished world, before sprite 66
  }
  if (s_dk_sprite) {
    dksprite::record_world(fb_w, fb_h);  // GL: sprite bucket 66 after merc/water
    dkdev::gpu_mark(1 + kPSprite);
  }
  if (s_dk_direct) {
    dkdirect::record_world(fb_w, fb_h);  // GL: debug 67, debug-no-zbuf 68, subtitle 69
    dkdev::gpu_mark(1 + kPDirect);
  }
  if (dkgl::active()) {
    // GL do_pcrtc_effects: the pmode_alp fade to black goes over everything, HUD included
    dkgl::record_blackout();
  }
}

bool dktfrag_present() {
  return dk_pass_on(g_game_version, "gk_dktfrag.txt");
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
    if (dk_pass_on(g_game_version, "gk_dkshrub.txt")) {
      s_dk_shrub = true;
      dkdev::set_world_recorder(record_tfrag_and_shrub);
      switch_run_logf("[dk] shrub world pass ON (gk_dkshrub.txt)");
    }
    if (dk_pass_on(g_game_version, "gk_dktie.txt")) {
      s_dk_tie = true;
      dkdev::set_world_recorder(record_tfrag_and_shrub);
      switch_run_logf("[dk] tie world pass ON (gk_dktie.txt)");
      snprintf(path, sizeof(path), "sdmc:/switch/%s/gk_nomerc.txt",
               game_version_names[g_game_version]);
      s_dk_merc = access(path, F_OK) != 0;
      switch_run_logf("[dk] merc pass %s", s_dk_merc ? "ON (with tie)" : "OFF (gk_nomerc.txt)");
      snprintf(path, sizeof(path), "sdmc:/switch/%s/gk_nosprite.txt",
               game_version_names[g_game_version]);
      s_dk_sprite = access(path, F_OK) != 0;
      switch_run_logf("[dk] sprite pass %s",
                      s_dk_sprite ? "ON (with tie)" : "OFF (gk_nosprite.txt)");
      snprintf(path, sizeof(path), "sdmc:/switch/%s/gk_nodirect.txt",
               game_version_names[g_game_version]);
      s_dk_direct = s_dk_sprite && g_game_version == GameVersion::Jak1 && access(path, F_OK) != 0;
      switch_run_logf("[dk] direct pass %s",
                      s_dk_direct ? "ON (with sprite)" : "OFF (gk_nodirect.txt / not jak1)");
      snprintf(path, sizeof(path), "sdmc:/switch/%s/gk_nosky.txt",
               game_version_names[g_game_version]);
      s_dk_sky = s_dk_direct && access(path, F_OK) != 0;
      switch_run_logf("[dk] sky pass %s",
                      s_dk_sky ? "ON (with direct)" : "OFF (gk_nosky.txt / no direct)");
      if (s_dk_sky) {
        dk_sky_init_textures();  // T88: register the blend outputs at SKY_TEXTURE_VRAM_ADDRS
      }
      snprintf(path, sizeof(path), "sdmc:/switch/%s/gk_noocean.txt",
               game_version_names[g_game_version]);
      s_dk_ocean = s_dk_tfrag && g_game_version == GameVersion::Jak1 &&
                   access(path, F_OK) != 0;
      switch_run_logf("[dk] ocean pass %s",
                      s_dk_ocean ? "ON (with tfrag)" : "OFF (gk_noocean.txt / not jak1)");
      if (s_dk_ocean && !dkgl::init(s_dk_tex_pool.get(), s_dk_loader.get())) {
        // T107: dkgl runs the unchanged GL ocean; this native path is the fallback
        // (gk_dkocean_native.txt, or any dkgl init failure -- logged as [dkgl]).
        // T95: ocean-texture render target in the pool at OCEAN_TEX_TBP_JAK1.
        dkocean::init_textures(s_dk_tex_pool.get(), g_game_version);
        dkocean::init_driver();
        // OCEAN_VISUAL_BUG (AI-assisted): gk_oceannomip.txt / gk_oceanlevels.txt.
        dkocean::init_debug_flags();
      }
    }
  }
}

void dk_exit() {
  // T60: stop recording the world before the Loader (and its level data) goes away.
  dkdev::set_world_recorder(nullptr);
  s_dk_tfrag = false;
  s_dk_shrub = false;
  s_dk_tie = false;
  s_dk_hfrag = false;
  s_dk_merc = false;
  s_dk_sprite = false;
  s_dk_direct = false;
  s_dk_sky = false;
  s_dk_ocean = false;
  for (int i = 0; i < 2; i++) {  // T88: the pool is going away with them
    s_sky_gpu[i] = nullptr;
    s_sky_tex_id[i] = 0;
  }
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
  // splash review (AI-assisted): the dk device is up ~3 s after boot, so the 3 s SplashTimer
  // (started at InitVideo) had nearly run out when the splash first appeared, then the screen
  // stayed black until the game's first frame (~18-20 s). Count from the first show and keep
  // it up until the first chain arrives.
  Timer m_splash_shown;
  float m_last_pmode_alp = 1.f;  // GL OpenGLRenderer::m_last_pmode_alp
  bool m_seen_chain = false;
  unsigned m_swap_interval = ~0u;  // force the first frame to apply it
  int m_op_mode = -1;             // docked/handheld state for P1.2b change logging (T09)
  bool m_focused = true;          // pause present while out of focus (T10 / P1.2b)

  struct Stats {
    int frames = 0, chains = 0;
    double wait = 0, record = 0, submit = 0, total = 0, max_total = 0;
    // T83: render-thread split + which part was largest in each slow (> 18 ms) frame
    double chain_wait = 0, capture = 0, loader = 0, build = 0, fence = 0, acquire = 0;
    int slow = 0, slow_ee = 0, slow_gpu = 0, slow_vsync = 0, slow_loader = 0, slow_cpu = 0;
    // T84: per-pass CPU build ms and GPU ms (sum, frames with data), max GPU frame
    double build_pass[kPassCount] = {}, gpu_pass[kPassCount] = {}, gpu_total = 0, gpu_max = 0;
    int gpu_frames = 0;
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
  m_splash_shown.start();
  dkdev::upload_splash(Gfx::g_splash.data.data(), Gfx::g_splash.width, Gfx::g_splash.height);
  Gfx::g_splash.data.clear();
  Gfx::g_splash.data.shrink_to_fit();
}

namespace {
// Wifi frame grab combo (AI-assisted): R3 + Plus held together arms a one-shot readback
// of the next presented frame, which present_frame streams to the wifi log host two
// frames later (dkdev::request_frame_grab -> switch_net_send_frame -> the
// frame-receiver.py listener on the dev machine). Same edge-triggered shape as the
// R3+Minus diagnostics combo in opengl.cpp, so holding the combo cannot re-arm it.
// This is the opt-in replacement for the removed FIX 115/117 SD-shot recorder.
void process_switch_grab_combo(SDL_GamepadButton button, bool down) {
  static unsigned s_held = 0;
  const unsigned kStart = 1u << (unsigned)SDL_GAMEPAD_BUTTON_START;
  const unsigned kRStick = 1u << (unsigned)SDL_GAMEPAD_BUTTON_RIGHT_STICK;
  const unsigned kCombo = kStart | kRStick;
  const unsigned bit = 1u << (unsigned)button;
  if ((bit & kCombo) == 0) {
    return;
  }
  if (down) {
    const bool was_complete = (s_held == kCombo);
    s_held |= bit;
    if (s_held == kCombo && !was_complete) {
      dkdev::request_frame_grab();
      switch_run_logf("[grab] R3+Plus: frame grab armed (streams to the wifi host)");
    }
  } else {
    s_held &= ~bit;
  }
}
}  // namespace

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
    // Watch gamepad buttons for the frame grab combo (same hook point as the GL path's
    // diagnostics toggle, opengl.cpp process_sdl_events).
    if (evt.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || evt.type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
      process_switch_grab_combo((SDL_GamepadButton)evt.gbutton.button,
                                evt.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
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
                    tex_ok ? "ok (96 MB image memblock, render thread owns uploads)"
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
  const bool show_splash = SplashScreen && m_splash_done &&
                           (m_splash_shown.getSeconds() < SPLASH_SCREEN_TIME || !m_seen_chain);

  // Engine handshake, same contract as render_game_frame() on the GL path: wait up to
  // 40 ms for a chain, then mark it consumed under dma_mutex (FIX 11). Phase 1 does not
  // draw it yet.
  bool got_chain = false;
  const u64 t_chain0 = dk_tick();
  {
    std::unique_lock<std::mutex> lock(g_dk->dma_mutex);
    got_chain = g_dk->dma_cv.wait_for(lock, std::chrono::milliseconds(40),
                                      [] { return g_dk->has_data_to_render; });
  }
  if (got_chain && !m_seen_chain) {
    m_seen_chain = true;  // splash review: the game draws from now on
    switch_run_logf("[dk] first chain: splash off after %.1f s on screen",
                    m_splash_shown.getSeconds());
  }
  const u64 t_cap0 = dk_tick();
  if (got_chain) {
    {
      std::unique_lock<std::mutex> lock(g_dk->sync_mutex);
      g_dk->frame_idx_of_input_data = g_dk->frame_idx;
    }
    std::unique_lock<std::mutex> lock(g_dk->dma_mutex);
    dk_drain_tex_ops();  // T80: before any capture looks textures up
    if (s_dk_tfrag) {
      // T60: read the chain BEFORE the ack below -- after it the EE rewrites that memory.
      dktf::capture_chain(g_dk->chain_data, g_dk->chain_offset);
      if (s_dk_hfrag) {
        dkhfrag::capture_chain(g_dk->chain_data, g_dk->chain_offset);  // T113/FIX 117
      }
      if (s_dk_shrub) {
        dkshrub::capture_chain(g_dk->chain_data, g_dk->chain_offset);  // T72
      }
      if (s_dk_tie) {
        dktie::capture_chain(g_dk->chain_data, g_dk->chain_offset);  // T76
      }
      if (s_dk_merc) {
        // T78: bones are read at their GOAL addresses, so pass the EE memory size.
        dkmerc::capture_chain(g_dk->chain_data, g_dk->chain_offset, EE_MAIN_MEM_SIZE);
      }
      if (s_dk_sprite) {
        // T79: replays the texture uploads (bucket order), then reads the sprite bucket.
        // FIX 112: the sky blend runs inside that replay (dk_sky_bucket_hook) so its
        // source textures are still in VRAM, like GL's bucket order.
        s_dk_upload_base = (const u8*)g_dk->chain_data;
        s_sky_hook_data = s_dk_sky ? g_dk->chain_data : nullptr;
        s_sky_hook_offset = g_dk->chain_offset;
        s_sky_hook_next = 0;
        dksprite::capture_chain(g_dk->chain_data, g_dk->chain_offset, dk_sprite_upload,
                                dk_sprite_lookup, dk_sky_bucket_hook);
        s_sky_hook_data = nullptr;
      }
      if (s_dk_sky) {
        if (!s_dk_sprite) {
          // no upload replay to hook into: blend against end-of-frame VRAM (old T88 path)
          dksky::capture_blend_chain(g_dk->chain_data, g_dk->chain_offset, dk_sky_source);
        }
        for (int i = 0; i < 2; i++) {
          // GL re-points the VRAM slots after every blend step (move_existing_to_vram);
          // once per frame, before the sky-draw capture resolves them, is enough here.
          if (s_sky_gpu[i] && s_dk_tex_pool) {
            s_dk_tex_pool->move_existing_to_vram(s_sky_gpu[i], SKY_TEXTURE_VRAM_ADDRS[i]);
          }
        }
        dkdirect::capture_sky_chain(g_dk->chain_data, g_dk->chain_offset, dk_direct_lookup);
      }
      if (s_dk_direct) {
        // T80: after sprite, so the pool holds this frame's uploads (font, HUD icons).
        dkdirect::capture_chain(g_dk->chain_data, g_dk->chain_offset, dk_direct_lookup);
      }
      if (s_dk_ocean) {
        // T95: after sprite + direct captures (the envmap tbp lookup happens at
        // build time; the far walk needs the dkdirect capture state ready).
        if (dkgl::active()) {
          // T107: the GL ocean renderers walk the chain now (it is acked right after).
          dkgl::capture_frame(g_dk->chain_data, g_dk->chain_offset);
        } else {
          dkocean::capture_chain(g_dk->chain_data, g_dk->chain_offset, dk_direct_lookup,
                                 s_dk_direct);
        }
      }
      if (s_dk_merc && s_dk_direct) {
        // T82: after sprite too (iris/pupil/lid textures come from this frame's uploads).
        dkeye::capture_chain(g_dk->chain_data, g_dk->chain_offset,
                             [](uint32_t tbp) { return dk_direct_lookup(tbp, false); });
      }
    }
    g_dk->has_data_to_render = false;
    g_dk->dma_cv.notify_all();
  }
  const u64 t_cap1 = dk_tick();

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
    // T49: render thread, before present. Same as OpenGLRenderer's loader step: a bigger
    // budget while the screen is black, and when the game fades back in, finish every
    // pending load first so the first visible frame has the whole level (fast blackout
    // loads). Without it the fade-in showed the level half loaded.
    const float alp = s_dk_pmode_alp.load(std::memory_order_relaxed);
    s_dk_loader->set_blackout(alp == 0.f);
    if (m_last_pmode_alp == 0.f && alp != 0.f) {
      const u64 t0 = dk_tick();
      s_dk_loader->update_blocking(*s_dk_tex_pool);
      switch_run_logf("[dk] fade-in: loader finished pending loads in %.1f ms",
                      dk_ms(t0, dk_tick()));
    } else {
      s_dk_loader->update(*s_dk_tex_pool);
    }
    m_last_pmode_alp = alp;
  }
  const u64 t_build0 = dk_tick();
  if (s_dk_tfrag && got_chain && focused) {
    // T60: before present_frame -- palette uploads reuse the frame cmd buffer. Only when
    // a present follows (the index ring relies on one present per build).
    u64 tp = dk_tick();
    auto lap = [&](int pass) {  // T84: CPU ms of the build just finished
      const u64 now = dk_tick();
      m_st.build_pass[pass] += dk_ms(tp, now);
      tp = now;
    };
    dktexgpu::begin_deferred_frame();  // T86: new staging slot for the tod palette copies
    if (s_dk_sky) {
      dksky::flush_blend(dk_sky_store);  // T88: this frame's sky/cloud blends into the pool
    }
    dktf::build_frame(s_dk_loader.get());
    if (s_dk_hfrag) {
      dkhfrag::build_frame(s_dk_loader.get());  // T113/FIX 117: jak3 only (empty on jak1)
    }
    lap(kPTfrag);
    if (s_dk_shrub) {
      dkshrub::build_frame(s_dk_loader.get());  // T72: own index ring + tod palettes
      lap(kPShrub);
    }
    if (s_dk_tie) {
      dktie::build_frame(s_dk_loader.get());  // T76: own index ring + tod palettes
      lap(kPTie);
    }
    if (s_dk_merc) {
      dkmerc::build_frame(s_dk_loader.get(), dktf::frame_draws().fog_rgba);  // T78
      lap(kPMerc);
    }
    if (s_dk_sprite) {
      dksprite::build_frame();  // T79: own vertex/index ring
      lap(kPSprite);
    }
    if (s_dk_direct) {
      dkdirect::build_frame(dktf::frame_draws().fog_rgba);  // T80: own vertex ring
      lap(kPDirect);
    }
    if (s_dk_merc && s_dk_direct) {
      dkeye::build_frame();  // T82
      lap(kPEye);
    }
    if (s_dk_ocean && !dkgl::active()) {
      dkocean::build_frame(dktf::frame_draws().fog_rgba);  // T95
    }
  }
  const u64 t_build1 = dk_tick();
  dkdev::FrameTiming ft{};
  if (focused) {
    dkdev::set_pcrtc(Gfx::g_global_settings.brightness_contrast_color,
                     Gfx::g_global_settings.brightness_contrast_alpha);
    ft = dkdev::present_frame(show_splash);
    dktexgpu::end_frame();  // T87: free textures released 3+ presents ago
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
    const double c_wait = dk_ms(t_chain0, t_cap0), c_cap = dk_ms(t_cap0, t_cap1),
                 c_load = dk_ms(t_cap1, t_build0), c_build = dk_ms(t_build0, t_build1);
    m_st.chain_wait += c_wait;
    m_st.capture += c_cap;
    m_st.loader += c_load;
    m_st.build += c_build;
    m_st.fence += ft.fence_ms;
    {
      // T84: GPU ms per pass from the timestamp marks (frame N-2, same slice). Mark 0 =
      // world start; pass p ends at mark 1+p, starts at the previous written mark.
      const double end = dkdev::gpu_mark_ms(dkdev::kGpuMarks - 1);
      if (end >= 0) {
        double prev = 0;
        for (int p = 0; p < kPassCount; p++) {
          const double m = dkdev::gpu_mark_ms(1 + p);
          if (m >= 0) {
            m_st.gpu_pass[p] += m - prev;
            prev = m;
          }
        }
        m_st.gpu_total += end;
        m_st.gpu_max = std::max(m_st.gpu_max, end);
        m_st.gpu_frames++;
      }
    }
    m_st.acquire += ft.acquire_ms;
    if (total > 18.0) {
      m_st.slow++;
      const double cpu = c_cap + c_build + ft.record_ms + ft.submit_ms;
      const double parts[5] = {c_wait, ft.fence_ms, ft.acquire_ms, c_load, cpu};
      int big = 0;
      for (int i = 1; i < 5; i++) {
        if (parts[i] > parts[big]) {
          big = i;
        }
      }
      int* slot[5] = {&m_st.slow_ee, &m_st.slow_gpu, &m_st.slow_vsync, &m_st.slow_loader,
                      &m_st.slow_cpu};
      (*slot[big])++;
    }
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
    {
      // T83: where the frame time goes. chainwait/ee = waiting for the game (GOAL) thread's
      // chain, gpu = slice fence,
      // vsync = image acquire; slow = frames over 18 ms by their largest part.
      switch_run_logf("[dk] split ms | chainwait %.2f capture %.2f loader %.2f build %.2f "
                      "gpufence %.2f vsync %.2f | slow %d: ee %d gpu %d vsync %d loader %d cpu %d",
                      m_st.chain_wait / n, m_st.capture / n, m_st.loader / n, m_st.build / n,
                      m_st.fence / n, m_st.acquire / n, m_st.slow,
                      m_st.slow_ee, m_st.slow_gpu, m_st.slow_vsync, m_st.slow_loader,
                      m_st.slow_cpu);
      // T84: per pass "cpu build / gpu" ms averages; gpu world = whole world pass on the GPU.
      char pl[384];
      int pn = 0;
      const double gf = m_st.gpu_frames ? (double)m_st.gpu_frames : 1.0;
      for (int p = 0; p < kPassCount && pn < (int)sizeof(pl) - 40; p++) {
        pn += snprintf(pl + pn, sizeof(pl) - pn, " %s %.2f/%.2f", kPassName[p],
                       m_st.build_pass[p] / n, m_st.gpu_pass[p] / gf);
      }
      // T86: time-of-day palette copies per frame (deferred) + waiting fallbacks since boot
      static uint64_t s_tod_last = 0;
      const uint64_t tod_now = dktexgpu::deferred_total();
      switch_run_logf("[dk] pass ms cpu/gpu |%s | gpu world %.2f (max %.1f, %d frames) | tod "
                      "%.1f/frame sync %llu",
                      pl, m_st.gpu_total / gf, m_st.gpu_max, m_st.gpu_frames,
                      (double)(tod_now - s_tod_last) / n,
                      (unsigned long long)dktexgpu::deferred_sync_fallbacks());
      s_tod_last = tod_now;
      if (s_dk_ocean) {
        // T95: ocean pass counters for this window.
        const dkocean::FlushStats& fs = dkocean::flush_stats();
        const dkocean::DriverStats& ds = dkocean::driver_stats();
        static dkocean::FlushStats s_f_last;
        static dkocean::DriverStats s_d_last;
        const double fwin = (double)(fs.frames - s_f_last.frames);
        switch_run_logf("[dk] ocean | texgen %.1f mip %.1f near %.1f mid %.1f per frame | nb %u,%u,%u "
                        "mb %u,%u/frame | no_tex %u "
                        "drop %u miss %u ubo %u | walks mf %u n %u mid_runs %u far %u "
                        "abort %u",
                        fwin > 0 ? (double)(fs.tex_gen_draws - s_f_last.tex_gen_draws) / fwin : 0,
                        fwin > 0 ? (double)(fs.tex_mip_draws - s_f_last.tex_mip_draws) / fwin : 0,
                        fwin > 0 ? (double)(fs.near_draws - s_f_last.near_draws) / fwin : 0,
                        fwin > 0 ? (double)(fs.mid_draws - s_f_last.mid_draws) / fwin : 0,
                        fwin > 0 ? (unsigned)((fs.near_bucket_draws[0] - s_f_last.near_bucket_draws[0]) / fwin) : 0,
                        fwin > 0 ? (unsigned)((fs.near_bucket_draws[1] - s_f_last.near_bucket_draws[1]) / fwin) : 0,
                        fwin > 0 ? (unsigned)((fs.near_bucket_draws[2] - s_f_last.near_bucket_draws[2]) / fwin) : 0,
                        fwin > 0 ? (unsigned)((fs.mid_bucket_draws[0] - s_f_last.mid_bucket_draws[0]) / fwin) : 0,
                        fwin > 0 ? (unsigned)((fs.mid_bucket_draws[1] - s_f_last.mid_bucket_draws[1]) / fwin) : 0,
                        fs.no_tex - s_f_last.no_tex, fs.dropped - s_f_last.dropped,
                        fs.misses - s_f_last.misses, fs.ubo_fail - s_f_last.ubo_fail,
                        ds.mid_far_walks - s_d_last.mid_far_walks,
                        ds.near_walks - s_d_last.near_walks,
                        ds.mid_runs - s_d_last.mid_runs, ds.far_packets - s_d_last.far_packets,
                        ds.mid_aborts - s_d_last.mid_aborts);
        s_f_last = fs;
        s_d_last = ds;
      }
      if (s_dk_sky) {
        const dksky::BlendStats& sb = dksky::blend_stats();
        switch_run_logf(
            "[dk] sky | pairs %u buckets %u draw %u/%u blend %u/%u | no-src %u (abs %u ph "
            "%u; %ux%u %ux%u) bad %u/%u/%u/%u/%u | store-fail %u | frames %llu",
            sb.pairs, sb.buckets, sb.sky_draws, sb.cloud_draws, sb.sky_blends, sb.cloud_blends,
            sb.no_src, sb.no_src_absent, sb.no_src_placeholder, sb.no_src_tbp[0],
            sb.no_src_tbp_count[0], sb.no_src_tbp[1], sb.no_src_tbp_count[1], sb.bad_adgif,
            sb.bad_alpha, sb.bad_draw, sb.bad_coord, sb.bad_size, s_sky_store_fails,
            (unsigned long long)sb.frames);
        if (sb.bad_size > 0 && s_dk_tex_pool) {
          GpuTexture* bt = s_dk_tex_pool->lookup_gpu_texture(sb.last_bad_tbp);
          switch_run_logf("[dk] sky bad-size | buf %d tbp %u bytes %u tex %ux%u '%s'",
                          sb.last_bad_idx, sb.last_bad_tbp, sb.last_bad_bytes,
                          bt ? bt->w : 0, bt ? bt->h : 0,
                          s_dk_tex_pool->get_debug_texture_name_from_tbp(sb.last_bad_tbp).c_str());
        }
      }
    }
    // T24 (P2.3 soak telemetry): one more [dk] line in the SAME report window as the
    // fps line just above (piggyback its exact timing/guard). Null-check each
    // subsystem so an inactive group prints "<name> -" instead of numbers; never
    // crash, never spam per frame (shares the report-window guard, so at most
    // every 2 s diag-on / 10 s diag-off, like the fps line).
    {
      const dkring::FrameRing* ring = dkdev::ubo_ring();
      const dkpool::BufferPool* pool = dkdev::buffer_pool();
      char tel[256];
      // tex: dktexgpu (T22) telemetry; active() = the 48 MB image memblock exists.
      if (dktexgpu::active()) {
        snprintf(tel, sizeof(tel),
                 "[dk] tex live=%u high=%u fail=%d freed=%llu mem=%lluKB holes=%lluKB | ",
                 dktexgpu::live_count(), dktexgpu::high_water(), dktexgpu::failed_allocs(),
                 (unsigned long long)dktexgpu::released_total(),
                 (unsigned long long)(dktexgpu::image_bytes_used() / 1024),
                 (unsigned long long)(dktexgpu::free_hole_bytes() / 1024));
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

// T80 (AI-assisted): GOAL upload-now! and texture-relocate (the jak1 font: upload, then a copy
// to the PSMT4HH slots that menu/HUD text samples). GOAL calls these on the EE thread, but
// dktexgpu is render-thread only, so they are queued in order and replayed by
// dk_drain_tex_ops() while the EE waits for the chain ack (its memory is stable then).
struct DkTexOp {
  const u8* tpage;  // nullptr = relocate
  int mode;
  u32 a, b, c;  // upload: s7 | relocate: dst, src, format
};
std::mutex s_dk_tex_ops_mutex;
std::vector<DkTexOp> s_dk_tex_ops;
constexpr size_t kMaxTexOps = 512;  // only grows while no pool drains it

void dk_texture_upload_now(const u8* tpage, int mode, u32 s7_ptr) {
  std::lock_guard<std::mutex> lk(s_dk_tex_ops_mutex);
  if (s_dk_tex_ops.size() < kMaxTexOps) {
    s_dk_tex_ops.push_back({tpage, mode, s7_ptr, 0, 0});
  }
}
void dk_texture_relocate(u32 dst, u32 src, u32 format) {
  std::lock_guard<std::mutex> lk(s_dk_tex_ops_mutex);
  if (s_dk_tex_ops.size() < kMaxTexOps) {
    s_dk_tex_ops.push_back({nullptr, 0, dst, src, format});
  }
}
void dk_drain_tex_ops() {
  std::vector<DkTexOp> ops;
  {
    std::lock_guard<std::mutex> lk(s_dk_tex_ops_mutex);
    ops.swap(s_dk_tex_ops);
  }
  if (!s_dk_tex_pool) {
    return;
  }
  for (const DkTexOp& op : ops) {
    if (op.tpage) {
      s_dk_tex_pool->handle_upload_now(op.tpage, op.mode, g_ee_main_mem, op.a, false);
    } else {
      // TexturePool::relocate ASSERTs on a missing source and reads gpu_textures[0].
      GpuTexture* src = s_dk_tex_pool->lookup_gpu_texture(op.b);
      if (src && !src->is_placeholder && !src->gpu_textures.empty()) {
        s_dk_tex_pool->relocate(op.a, op.b, op.c);
        switch_run_logf("[dk] tex relocate %u -> %u fmt %u ok", op.b, op.a, op.c);
      } else {
        switch_run_logf("[dk] tex relocate %u -> %u fmt %u: source missing", op.b, op.a, op.c);
      }
    }
  }
}
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
// GL gl_set_pmode_alp: GOAL's blackout (0 = black screen while a level loads, 1 = visible).
void dk_set_pmode_alp(float val) {
  s_dk_pmode_alp.store(val, std::memory_order_relaxed);
  dkgl::set_pmode_alp(val);
}

}  // namespace

namespace dk_renderer {
bool kill_switch_present(GameVersion version) {
  char path[96];
  snprintf(path, sizeof(path), "sdmc:/switch/%s/gk_opengl.txt", game_version_names[version]);
  if (access(path, F_OK) == 0) {
    return false;  // user opt-out: plain OpenGL renderer
  }
  return dk_pass_on(version, "gk_dk3d.txt");
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
