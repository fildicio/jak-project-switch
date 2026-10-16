# T23 — TexturePool seam: GOAL texture path → dk behind the kill-switch (P2.3)

**Prerequisite: T22 must be ✅ reviewed first.** This task makes the GOAL runtime
texture path (module `texture_upload_now` / `texture_relocate`, then
`TexturePool::give_texture*` → `update_gl_texture` → `upload_to_gpu`) upload into
TexturePoolDeko3D instead of OpenGL when the dk renderer is active.

## Read first
- `docs/deko3d/AGENT_KNOWLEDGE_MAP.md` §7 (Textures, L138–149).
- `game/graphics/texture/TexturePool.h` + `.cpp` end to end — note `GpuTexture`,
  `TextureReference`, `give_texture`, `give_texture_and_load_to_vram`,
  `update_gl_texture`, `get_gpu_texture_for_slot`, `upload_to_gpu` (GL-specific, ~L39–62;
  the `glTexImage2D` is at .cpp:61).

## Hard constraints
- `game/graphics/texture/TexturePool.{h,cpp}` is SHARED game code (not under
  `opengl_renderer/`) — it MAY be edited, but every change must sit inside
  `#if defined(__SWITCH__)` guards so the desktop build compiles to IDENTICAL code
  (byte-for-byte desktop is a project rule; the reviewer will verify it).
- NEVER edit `game/graphics/opengl_renderer/` (LoaderStages BCn path included — level
  BCn uploads stay GL for now and are P3's problem; document that in a comment).
- Keep `GpuTexture` bookkeeping (slot ids, w/h/mips) filled exactly as the GL path does,
  so GOAL handoff and every reader are unchanged. Only the bytes' destination changes.
- STOP rule: on any command failure append ONLY the last 5 lines, no self-fixing, report STOP.

## Edits

### `game/graphics/texture/TexturePool.cpp`
1. Near the top, inside `#if defined(__SWITCH__)`:
   `#include "game/graphics/pipelines/dk_device.h"` (dkdev::is_up) and
   `#include "game/graphics/deko3d_renderer/dk_texture_gpu.h"` (dktexgpu).
2. In the texture upload path that currently ends at `glTexImage2D` (.cpp:61): wrap the
   GL call so that on Switch, when `dkdev::is_up() && dktexgpu::active()`, the pixels go
   to `dktexgpu::upload_rgba8(w, h, data)` instead; store the returned dktex id in the
   GpuTexture (add an id field ONLY inside a `#if defined(__SWITCH__)` block in the
   header), keep filling every existing field identically. A 0 return → skip the upload
   (FIX 33 caller contract), log via `switch_run_logf("[dk] tex upload skipped (pool full)")`.
3. Ref-count mapping: where the GL path bumps/drops level refs (give_texture /
   TextureReference release), call `dktexgpu::add_level_ref(id)` / `dktexgpu::release(id)`
   inside the same guards, so FIX 33 dedup ("same texture in two levels = one upload")
   holds on the dk path too.
4. Leave the swapchain/splash/renderer decision logic untouched — the kill-switch
   (`gfx.cpp:197`, `gk_dk3d.txt`) already decided dk before any of this runs.

## Done-check (background wrappers ONLY)
```
scripts/ai/check-bg.sh jak1
scripts/ai/check-tail.sh
```
QWEN_REPORT `## T23`: files changed, a short mapping table (GL call → dk call), last 5
log lines + md5, and the exact grep proving every edit is inside `__SWITCH__` guards.
Flip README row to 🟡. No commit, no deploy. The desktop build + first hardware boot are
the REVIEWER's/user's steps.
