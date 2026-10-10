/*!
 * @file gfx.cpp
 * Graphics component for the runtime. Abstraction layer for the main graphics routines.
 */

#include "gfx.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <utility>

#include "display.h"

#include "common/global_profiler/GlobalProfiler.h"
#include "common/log/log.h"
// FIX 70 / FIX 55 pattern (AI-assisted): Switch-only core pinning helpers,
// empty off-Switch.
#include "game/switch/platform.h"
#include "common/symbols.h"
#include "common/util/FileUtil.h"
#include "common/util/json_util.h"

#include "game/common/file_paths.h"
#include "game/kernel/common/kmachine.h"
#include "game/kernel/common/kscheme.h"
#include "game/runtime.h"
#include "pipelines/opengl.h"

namespace Gfx {

std::function<void()> vsync_callback;
GfxGlobalSettings g_global_settings;
game_settings::DebugSettings g_debug_settings;
SplashScreen g_splash;

// Port credit (AI-assisted): tiny 5x7 pixel font + compositor for the boot-splash
// credit stamp. The splash image is a data file (SCREEN1.*) that anyone can swap,
// so the "made by fildicio" line is drawn here, in compiled code -- stripping it
// means rebuilding the binary. Rows are 5-bit masks, bit 4 (0x10) = leftmost pixel,
// and only the characters the credit line needs are defined (default: blank).
namespace {
constexpr int kCreditGlyphW = 5;
constexpr int kCreditGlyphH = 7;
constexpr int kCreditAdvance = 6;  // 5 px glyph + 1 px gap
constexpr int kCreditScale = 2;
constexpr int kCreditMargin = 12;
constexpr const char* kSplashCreditText = "made by fildicio";

constexpr u8 kGlyphSpace[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
constexpr u8 kGlyphA[7] = {0x00, 0x00, 0x0E, 0x01, 0x0F, 0x11, 0x0E};
constexpr u8 kGlyphB[7] = {0x1C, 0x12, 0x1C, 0x11, 0x11, 0x11, 0x0E};
constexpr u8 kGlyphC[7] = {0x00, 0x00, 0x0E, 0x11, 0x10, 0x11, 0x0E};
constexpr u8 kGlyphD[7] = {0x01, 0x02, 0x0E, 0x11, 0x11, 0x11, 0x0E};
constexpr u8 kGlyphE[7] = {0x00, 0x00, 0x0E, 0x11, 0x1F, 0x10, 0x0E};
constexpr u8 kGlyphF[7] = {0x06, 0x08, 0x1C, 0x08, 0x08, 0x08, 0x08};
constexpr u8 kGlyphI[7] = {0x04, 0x00, 0x04, 0x04, 0x04, 0x04, 0x04};
constexpr u8 kGlyphL[7] = {0x0C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E};
constexpr u8 kGlyphM[7] = {0x00, 0x00, 0x1F, 0x15, 0x15, 0x15, 0x15};
constexpr u8 kGlyphO[7] = {0x00, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E};
constexpr u8 kGlyphY[7] = {0x00, 0x00, 0x11, 0x11, 0x11, 0x0F, 0x01};

const u8* credit_glyph(char c) {
  switch (c) {
    case ' ':
      return kGlyphSpace;
    case 'a':
      return kGlyphA;
    case 'b':
      return kGlyphB;
    case 'c':
      return kGlyphC;
    case 'd':
      return kGlyphD;
    case 'e':
      return kGlyphE;
    case 'f':
      return kGlyphF;
    case 'i':
      return kGlyphI;
    case 'l':
      return kGlyphL;
    case 'm':
      return kGlyphM;
    case 'o':
      return kGlyphO;
    case 'y':
      return kGlyphY;
    default:
      return kGlyphSpace;
  }
}
}  // namespace

void stamp_splash_credit(std::vector<u8>& data, int width, int height) {
  if (width <= 0 || height <= 0 || (int)data.size() < width * height * 4) {
    return;
  }
  int text_chars = 0;
  for (const char* p = kSplashCreditText; *p; ++p) {
    text_chars++;
  }
  const int text_w = text_chars * kCreditAdvance * kCreditScale;
  const int text_h = kCreditGlyphH * kCreditScale;
  // Too small / weird splash (demo screens etc.) -- skip rather than corrupt pixels.
  if (width < text_w + kCreditMargin || height < text_h + kCreditMargin) {
    return;
  }
  const int x0 = kCreditMargin;
  const int y0 = height - kCreditMargin - text_h;

  auto fill_rect = [&data, width, height](int x, int y, int w, int h, u8 r, u8 g, u8 b) {
    const int x1 = std::min(x + w, width);
    const int y1 = std::min(y + h, height);
    for (int yy = std::max(y, 0); yy < y1; yy++) {
      for (int xx = std::max(x, 0); xx < x1; xx++) {
        u8* px = &data[((size_t)yy * (size_t)width + (size_t)xx) * 4];
        px[0] = r;
        px[1] = g;
        px[2] = b;
        px[3] = 255;
      }
    }
  };

  // Two passes: black outline (grown 1 px) first, then the white glyph fill, so the
  // line stays legible on any splash background.
  for (int pass = 0; pass < 2; pass++) {
    const u8 color = (pass == 0) ? 0x00 : 0xFF;
    const int grow = (pass == 0) ? 1 : 0;
    int cx = x0;
    for (const char* p = kSplashCreditText; *p; ++p) {
      const u8* glyph = credit_glyph(*p);
      for (int gy = 0; gy < kCreditGlyphH; gy++) {
        const u8 row = glyph[gy];
        if (!row) {
          continue;
        }
        for (int gx = 0; gx < kCreditGlyphW; gx++) {
          if (row & (0x10 >> gx)) {
            fill_rect(cx + gx * kCreditScale - grow, y0 + gy * kCreditScale - grow,
                      kCreditScale + grow * 2, kCreditScale + grow * 2, color, color, color);
          }
        }
      }
      cx += kCreditAdvance * kCreditScale;
    }
  }
  lg::info("splash: stamped port credit\n");
}

const GfxRendererModule* GetRenderer(GfxPipeline pipeline) {
  switch (pipeline) {
    case GfxPipeline::Invalid:
      lg::error("Requested invalid renderer", fmt::underlying(pipeline));
      return NULL;
    case GfxPipeline::OpenGL:
      return &gRendererOpenGL;
    default:
      lg::error("Requested unknown renderer {}", fmt::underlying(pipeline));
      return NULL;
  }
}

void SetRenderer(GfxPipeline pipeline) {
  g_global_settings.renderer = GetRenderer(pipeline);
}

const GfxRendererModule* GetCurrentRenderer() {
  return g_global_settings.renderer;
}

u32 Init(GameVersion version) {
  lg::info("GFX Init");
  prof().instant_event("ROOT");

  g_debug_settings = game_settings::DebugSettings();
  g_debug_settings.load_settings();
  {
    auto p = scoped_prof("startup::gfx::get_renderer");
    g_global_settings.renderer = GetRenderer(GfxPipeline::OpenGL);
  }

  {
    auto p = scoped_prof("startup::gfx::init_current_renderer");
    if (GetCurrentRenderer()->init(g_global_settings)) {
      lg::error("Gfx::Init error");
      return 1;
    }
  }

  if (g_main_thread_id != std::this_thread::get_id()) {
    lg::error("Ran Gfx::Init outside main thread. Init display elsewhere?");
  } else {
    {
      auto p = scoped_prof("startup::gfx::init_main_display");
      std::string title = "OpenGOAL";
      if (g_game_version == GameVersion::Jak2 || g_game_version == GameVersion::Jak3 ||
          g_game_version == GameVersion::JakX) {
        title += " - Work in Progress";
      }
      title += fmt::format(" - {} - {}", version_to_game_name_external(g_game_version),
                           build_revision());
      Display::InitMainDisplay(640, 480, title.c_str(), g_global_settings, version);
    }
  }

  return 0;
}

void Loop(std::function<bool()> f) {
  lg::info("GFX Loop");
#ifdef __SWITCH__
  // FIX 70 (AI-assisted): this IS the main/render thread -- pin it to core 1
  // (PERF_PLAN_NEXT_AGENT.md step 1) so GOAL logic (core 0) and the
  // loader/audio workers (core 2) can never preempt the frame.
  switch_platform::switch_pin_current_thread("render", 1);
#endif
  while (f()) {
#ifdef __SWITCH__
    // FIX 70: at most one [cores] line every 10 s; switch_run_logf batches
    // its writes (FIX 40), so this is safe mid-stream.
    switch_platform::switch_core_diag_periodic("render");
#endif
    auto p = scoped_prof("gfx loop");
    // check if we have a display
    if (Display::GetMainDisplay()) {
      Display::GetMainDisplay()->render();
    }
  }
}

u32 Exit() {
  lg::info("GFX Exit");
  Display::KillMainDisplay();
  GetCurrentRenderer()->exit();
  g_debug_settings.save_settings();
  return 0;
}

void register_vsync_callback(std::function<void()> f) {
  vsync_callback = std::move(f);
}

void clear_vsync_callback() {
  vsync_callback = nullptr;
}

u32 vsync() {
  if (GetCurrentRenderer()) {
    // Inform the IOP kernel that we're vsyncing so it can run the vblank handler
    if (vsync_callback != nullptr)
      vsync_callback();
    return GetCurrentRenderer()->vsync();
  }
  return 0;
}

u32 sync_path() {
  if (GetCurrentRenderer()) {
    return GetCurrentRenderer()->sync_path();
  }
  return 0;
}

bool CollisionRendererGetMask(GfxGlobalSettings::CollisionRendererMode mode, s64 mask_id) {
  int arr_idx = mask_id / 32;
  int arr_ofs = mask_id % 32;

  switch (mode) {
    case GfxGlobalSettings::CollisionRendererMode::Mode:
      return (g_global_settings.collision_mode_mask[arr_idx] >> arr_ofs) & 1;
    case GfxGlobalSettings::CollisionRendererMode::Event:
      return (g_global_settings.collision_event_mask[arr_idx] >> arr_ofs) & 1;
    case GfxGlobalSettings::CollisionRendererMode::Material:
      return (g_global_settings.collision_material_mask[arr_idx] >> arr_ofs) & 1;
    case GfxGlobalSettings::CollisionRendererMode::Skip:
      if (mask_id == -1) {
        return g_global_settings.collision_skip_nomask_allowed;
      } else {
        return g_global_settings.collision_skip_mask & mask_id;
      }
    case GfxGlobalSettings::CollisionRendererMode::SkipHide:
      return g_global_settings.collision_skip_hide_mask & mask_id;
    default:
      lg::error("{} invalid params {} {}", __PRETTY_FUNCTION__, fmt::underlying(mode), mask_id);
      return false;
  }
}

void CollisionRendererSetMask(GfxGlobalSettings::CollisionRendererMode mode, s64 mask_id) {
  int arr_idx = mask_id / 32;
  int arr_ofs = mask_id % 32;

  switch (mode) {
    case GfxGlobalSettings::CollisionRendererMode::Mode:
      g_global_settings.collision_mode_mask[arr_idx] |= 1 << arr_ofs;
      break;
    case GfxGlobalSettings::CollisionRendererMode::Event:
      g_global_settings.collision_event_mask[arr_idx] |= 1 << arr_ofs;
      break;
    case GfxGlobalSettings::CollisionRendererMode::Material:
      g_global_settings.collision_material_mask[arr_idx] |= 1 << arr_ofs;
      break;
    case GfxGlobalSettings::CollisionRendererMode::Skip:
      if (mask_id == -1) {
        g_global_settings.collision_skip_nomask_allowed = true;
      } else {
        g_global_settings.collision_skip_mask |= mask_id;
      }
      break;
    case GfxGlobalSettings::CollisionRendererMode::SkipHide:
      g_global_settings.collision_skip_hide_mask |= mask_id;
      break;
    default:
      lg::error("{} invalid params {} {}", __PRETTY_FUNCTION__, fmt::underlying(mode), mask_id);
      break;
  }
}

void CollisionRendererClearMask(GfxGlobalSettings::CollisionRendererMode mode, s64 mask_id) {
  int arr_idx = mask_id / 32;
  int arr_ofs = mask_id % 32;

  switch (mode) {
    case GfxGlobalSettings::CollisionRendererMode::Mode:
      g_global_settings.collision_mode_mask[arr_idx] &= ~(1 << arr_ofs);
      break;
    case GfxGlobalSettings::CollisionRendererMode::Event:
      g_global_settings.collision_event_mask[arr_idx] &= ~(1 << arr_ofs);
      break;
    case GfxGlobalSettings::CollisionRendererMode::Material:
      g_global_settings.collision_material_mask[arr_idx] &= ~(1 << arr_ofs);
      break;
    case GfxGlobalSettings::CollisionRendererMode::Skip:
      if (mask_id == -1) {
        g_global_settings.collision_skip_nomask_allowed = false;
      } else {
        g_global_settings.collision_skip_mask &= ~mask_id;
      }
      break;
    case GfxGlobalSettings::CollisionRendererMode::SkipHide:
      g_global_settings.collision_skip_hide_mask &= ~mask_id;
      break;
    default:
      lg::error("{} invalid params {} {}", __PRETTY_FUNCTION__, fmt::underlying(mode), mask_id);
      break;
  }
}

void CollisionRendererSetMode(GfxGlobalSettings::CollisionRendererMode mode) {
  g_global_settings.collision_mode = mode;
}

}  // namespace Gfx
