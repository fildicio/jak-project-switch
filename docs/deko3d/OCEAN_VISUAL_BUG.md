# Ocean visual bug: text description for text-only models (AI-assisted)

GLM cannot read images. NEVER open `.png`/`.jpg`/`.heic` files: one image kills the session
(`messages.content.type is invalid`). This file describes the hardware photos in words.

## What the Switch shows (jak1, deko3d path, handheld, 2026-10-24)

Six photos of the sea near Sandover / Misty dock, from the human tester:

1. **Ocean drawn over things above the water.** Jak standing on sand and the wooden dock
   are tinted strong blue, as if a semi-transparent blue layer were drawn over them.
2. **Black slab at distance.** From the beach looking out to sea, everything below the
   horizon is one opaque black/dark-navy area. It covers the lower part of the cliffs.
   Distant islands on the horizon appear as solid black silhouettes with blocky edges,
   with sky colour visible in the gaps between them.
3. **Saturated blue streaks near the camera.** Close water is pure saturated blue with
   long straight streaks parallel to the horizon (texture stretched along one axis).
4. **Huge straight-edged triangles.** Large flat polygons reach the screen edges; one has
   a V-shaped lower edge with black below it. Hard diagonal seams across the water.
5. Sky, stars, Jak's model, the HUD and the dock geometry render fine apart from the tint.
   The bug depends on distance: black far away, streaks at middle range.

## Already checked (correct, don't re-investigate)

- Draw order: `dk_renderer.cpp` records ocean mid/far after sky, before tfrag (GL bucket 4),
  and ocean-near at bucket 63. Same as GL.
- Mid pass depth ALWAYS + write (FIX 114): matches GL `CommonOceanRenderer::flush_mid`.
- Primitive restart: `dkCmdBufSetPrimitiveRestart(cb, true, UINT32_MAX)` in
  `dk_ocean_flush.cpp` (~line 574); indices use `UINT32_MAX` like GL.

## Suspects, most likely first

1. **Ocean texture mip chain** (`dk_ocean_texture.cpp`, ocean-mipmap pass; log line
   `ocean tex target at tbp 8160: id 352 (mips 8 ...)`). If mip levels 1..7 are
   unwritten, written without a barrier before sampling, or the image view/sampler
   covers the wrong level range, far water samples garbage (black) and mid-range samples
   stretched data (streaks). `dk_ocean_flush.cpp` samples with mip filtering
   (`find_sampler(..., mip, ...)`, FIX 112).
2. **Stock deko3d alpha bug** (`third-party/deko3d/PATCHES.md` patch 1): `FuncAlphaDst`
   gets `dstColorBlendFactor`. The ocean flush StdAlpha/AlphaOnly passes write alpha that
   the envmap pass reads with `DkBlendFactor_DstAlpha`, so wrong alpha means wrong
   envmap. Compare `gk-stockdk.nro` against the patched `gk.nro`.
3. **Vertex/UV decode** in `dk_ocean_common.cpp` (`xyz = (x<<16)/UINT32_MAX` etc.,
   STQ/q handling) compared with `opengl_renderer/ocean/CommonOceanRenderer.cpp`. A bad q
   gives stretched textures and giant triangles.

## Requested work (text-only, code + host checks)

1. Diff `game/graphics/deko3d_renderer/dk_ocean_{common,texture,flush,mid,near}.cpp`
   against `game/graphics/opengl_renderer/ocean/*.cpp`, line by line. List every
   behavioural difference (state, mip levels, texture-gen viewport, barriers, UV/q math).
2. Add SD flag `sdmc:/gk_oceannomip.txt`: when present, ocean sampling uses no mip filter
   (level 0 only). Read it once at init like the other `gk_*.txt` flags, log one line
   `[dk] ocean mip sampling OFF (gk_oceannomip.txt)`. Off by default.
3. Add SD flag `sdmc:/gk_oceanlevels.txt`: after the mipmap pass, log once per 600 frames
   the average RGBA of each mip level of the ocean texture (CPU readback into a small
   CPU-visible memblock after a fence; first-use allocation only). This turns
   "black/streaky" into numbers a text model can read.
4. Build `scripts/ai/check-switch.sh jak1` (must PASS) and `python3 scripts/ai/check-dk.py`
   (must PASS). Do not deploy. Report the diff findings and which hardware test to run.

Rules: `#ifdef __SWITCH__` / deko3d path only, no edits in `opengl_renderer/`, no new
runtime threads, API names checked in `docs/deko3d-ref/include/deko3d.h`.
