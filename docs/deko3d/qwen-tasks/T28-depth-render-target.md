# T28 — dkrt depth render target (P3.1 slice 2, AI-assisted)

Track D P3.1 · Depends on: T27 ✅ · Inert (no new caller) · One session

## Goal

Add a depth attachment to `dkrt` (T27's color render target): a Z24S8 image in its
own memblock, bound as the depth target of `dkCmdBufBindRenderTarget`. This is the
deko3d equivalent of the GL `zbuf_stencil_id` renderbuffer you can see in
`game/graphics/opengl_renderer/Fbo.h:13`.

## Context (read these first)

- `game/graphics/deko3d_renderer/dk_render_target.h` — the T27 API you are extending.
- `game/graphics/deko3d_renderer/dk_render_target.cpp` — copy its image/memblock
  pattern exactly (layout maker → size/alignment → own 0x1000-rounded
  `GpuCached|Image` memblock → image at offset 0).
- `game/graphics/opengl_renderer/Fbo.h:12-16` — GL names color + optional depth.

## API cheat-sheet (verified against docs/deko3d-ref/include/deko3d.h)

- Formats (L425-429): `DkImageFormat_Z24S8` (depth24+stencil8, matches GL), also
  `DkImageFormat_ZF32`. **Use Z24S8.**
- `DkImageLayoutMaker` (grep `typedef struct DkImageLayoutMaker`): has `format`,
  `flags`, `msMode` (set `DkMsMode_1x`, enum L528-534), `dimensions[3]`.
- Flags: `DkImageFlags_UsageRender` (1U<<8). No 2D-engine usage needed for depth.
- Bind (the static inline T27 already uses, ~L1386): `dkCmdBufBindRenderTarget(cb,
  &colorView, &depthView)` — depth is the third arg, was `nullptr` until now.
- Depth clear (used later by T38, not here): `dkCmdBufClearDepthStencil`
  (deko3d.h:1302).

## Steps

1. Extend `dkrt::Desc` with `bool with_depth = false;` (default keeps old callers
   byte-compatible).
2. In `init()`: when `with_depth`, run the same layout/memblock/image dance for a
   Z24S8 image of the same dimensions, in a SECOND memblock owned by dkrt (do not
   mix both images into one memblock — keep T27's color block untouched).
3. Extend `bind()` to pass `&s_depthView` when the depth image exists, else
   `nullptr` (today's behaviour).
4. `teardown()`/`valid()` must cover the depth memblock too. `width()/`height()`
   unchanged.
5. Header comment: bump the "slice" note (slice 1 → slices 1-2), keep the
   plain-header rule (no `<deko3d.h>`, `cstdint` only).

## Rules

- Only touch `dk_render_target.h` / `dk_render_target.cpp` (already in CMake).
- Check every `dkMemBlockCreate` result; on failure log ONE
  `[dk] rt depth init failed (w x h, N MB)` line, `teardown()` the color half too,
  return false. Never crash.
- Whole .cpp body stays inside `#if defined(__SWITCH__)`.

## Done-check

```sh
scripts/ai/check-bg.sh jak1 && scripts/ai/check-tail.sh   # repeat until DONE-PASS / DONE-FAIL
```

Paste the last lines. Expected: DONE-PASS and (because nothing calls the new code
yet and `--gc-sections` strips it) the NRO md5 equals the current inert baseline
`74635895c2d04602de48dc5dccd1839e`. (**Re-baselined by the reviewer 2026-10-17
night:** the T24-era `727feb1c…` had drifted — the reviewer rebuilt BOTH the clean
HEAD tree and the T28 tree and both are byte-identical at `74635895…`, proving T28
inert; verified DONE-PASS `PASS: jak1 nro=build-switch-jak1/game/gk.nro
md5=74635895c2d04602de48dc5dccd1839e`.)
If the md5 CHANGED from `74635895…`, something is no longer inert — say so and stop.
(If T43 already landed when you run this, ask the reviewer for the new baseline.)

## Doubts

- If Z24S8 layout creation fails on hardware layoutmaker (unlikely — it is a
  standard format), do NOT silently switch format: report back.
