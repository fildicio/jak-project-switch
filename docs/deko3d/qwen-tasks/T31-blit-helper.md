> **SPLIT 2026-10-18** — halved for the coder model. Superseded by
> `T31a-blit-core.md` + `T31b-blit-grounding.md`; status lives on those rows.
> Kept for history — do NOT run this file.

# T31 — dkrt blit helper (P3.1 slice 5, AI-assisted)

Track D P3.1 · Depends on: T29b ✅ (T30 optional) · Inert · One session

## Goal

`dkrt::blit()` — the deko3d equivalent of the 7 `glBlitFramebuffer` call sites
(the plan's P3.1 inventory: 9 framebuffers, 7 blits, 1 readpixels). One wrapper,
rect-based, with the GL filter semantics mapped. The splash path already blits
(`dk_device.cpp:612`) — this task factors that knowledge into dkrt and extends it
with the modes the game's blits need.

## Context

- `game/graphics/pipelines/dk_device.cpp:606-613` — the existing blit: view
  defaults, `DkImageRect` src/dst, `dkCmdBufBarrier(DkBarrier_Full, 0)` before it
  (3D→2D engine), `DkBlitFlag_FilterLinear`. Read it first.
- `game/graphics/opengl_renderer/BlitDisplays.cpp` — run
  `grep -n 'glBlitFramebuffer' -B4 game/graphics/opengl_renderer/BlitDisplays.cpp`
  and note every filter/mask combination actually used; put the list in the
  header comment (this grounds "7 blits").
- `grep -rn 'glBlitFramebuffer' game/graphics/opengl_renderer/ | grep -v BlitDisplays`
  for any other site; list them too.

## API cheat-sheet (verified against docs/deko3d-ref/include/deko3d.h)

- `dkCmdBufBlitImage(cb, &srcView, &srcRect, &dstView, &dstRect, flags, factor)`
  — L1316.
- `DkImageRect` = `{x, y, z, w, h, depth}` (see dk_device.cpp:608 usage).
- Flags (L1171-1183): `DkBlitFlag_FilterLinear` / `FilterNearest`,
  `DkBlitFlag_FlipX`/`FlipY` (grep the enum for exact names — FlipX is L1172).
- GL map: `GL_LINEAR` → FilterLinear; `GL_NEAREST` → FilterNearest;
  `glBlitFramebuffer` mask GL_COLOR_BUFFER_BIT → the whole-image semantics above
  (deko3d blits are always color); no scissor flag exists — out of scope.

## Steps

1. Add to dk_render_target:
   ```cpp
   enum class BlitFilter { Nearest, Linear };
   // dst/src given as views + rects; caller owns barrier policy via `with_barrier`
   bool blit(const DkImageView* src, const DkImageRect* src_rect,
             const DkImageView* dst, const DkImageRect* dst_rect,
             BlitFilter filter, bool flip_x, bool flip_y);
   ```
   (the .h stays plain: forward-declare nothing — put this overload ONLY in the
   .cpp and expose a plain struct-based signature in the .h instead, exactly like
   dk_device_internal.h does for Dk handles: small opaque `struct RtView;`
   wrapper OR a deko3d-free `struct Rect {uint32_t x,y,w,h;}` signature. Pick the
   plain-struct route.)
2. Implementation composes flag bits; records `dkCmdBufBarrier` +
   `dkCmdBufBlitImage` on the current cmd buf (same order as dk_device.cpp:611-12).
3. Header comment: the grounded list of GL blit sites + which filter each uses.

## Rules

- Only `dk_render_target.h/.cpp`. Plain header stays `<deko3d.h>`-free — if you
   cannot express the signature without deko3d types, put the function in
   `dk_state.cpp`-style Switch-only territory and STOP to say why.
- One `[dk]` failure line per bad input (null views); never crash.

## Done-check

```sh
scripts/ai/check-bg.sh jak1 && scripts/ai/check-tail.sh
```

DONE-PASS + NRO md5 unchanged (`08fc9510…`). Changed → not inert, stop.

## Doubts

- If a game blit needs stretch + flip simultaneously, note it — deko3d composes
  flags, but confirm on the L1171-1183 comment text before claiming it.
