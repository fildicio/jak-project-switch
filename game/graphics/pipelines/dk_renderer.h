#pragma once

/*!
 * @file dk_renderer.h
 * deko3d migration Phase 1 (AI-assisted): native deko3d renderer module for the Switch.
 *
 * Selected at boot by default for jak1 (sdmc:/switch/jak1/gk_opengl.txt opts out) and,
 * for jak2/jak3, only when sdmc:/switch/<game>/gk_dk3d.txt exists; otherwise the
 * OpenGL/Mesa path runs exactly as before. Named dk_*
 * so it can never be confused with libnx's <deko3d.h>.
 *
 * Phase 1 scope (DEKO3D_MIGRATION_PLAN.md P1.1-P1.5): device, memory blocks, queue,
 * swapchain on the native window (SDL video is never initialized on this path -- SDL
 * stays for gamepads/events only), boot splash presented via the 2D engine, the GOAL
 * engine handshake (vsync / sync_path / send_chain) so the game runs, [dk] + [heap]
 * telemetry. Game frames are consumed but not drawn yet (that is P2/P3).
 */

#include "game/graphics/gfx.h"

#if defined(__SWITCH__)
extern const GfxRendererModule gRendererDeko3D;

namespace dk_renderer {
//! True when deko3d should be tried: jak1 unless gk_opengl.txt exists; jak2/jak3 only
//! with gk_dk3d.txt (checked once per call, boot only).
bool kill_switch_present(GameVersion version);
}  // namespace dk_renderer
#endif
