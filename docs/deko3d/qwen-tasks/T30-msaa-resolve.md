# T30 — dkrt MSAA + resolve (P3.1 slice 4, AI-assisted)

Track D P3.1 · Depends on: T29b ✅ · Inert · One session

## Goal

Support multisampled render targets and a one-call resolve, the deko3d equivalent
of the GL `resolve_buffer` in `FboState` (OpenGLRenderer.h:158) +
`glBlitFramebuffer` resolve. Switch GL runs MSAA only when
`Gfx::g_global_settings.msaa_samples > 1` — the helper takes the sample count as
a parameter and must accept 1 as "no-op mode" (bind + straight copy path).

## Context

- `game/graphics/opengl_renderer/OpenGLRenderer.h:154-161` — `render_buffer`
  (msaa) + `resolve_buffer` (1x) pattern you are reproducing.
- `docs/deko3d-ref/examples/deko_examples/source/Example5*.cpp` — the official
  MSAA example; read it before writing anything (pattern: msaa image with
  `UsageRender`, 1x image with `Usage2DEngine`-style blit usage, resolve, then
  the 1x image is what gets sampled/blitted onward).

## API cheat-sheet (verified against docs/deko3d-ref/include/deko3d.h)

- `DkMsMode` enum L528-534: `DkMsMode_1x=0, 2x=1, 4x=2, 8x=3`.
- `DkImageLayoutMaker` has a `DkMsMode msMode` field (grep the struct) — this is
  how an image becomes multisampled. Resolve DESTINATION stays `DkMsMode_1x`.
- `dkCmdBufResolveImage(cb, &srcView, &dstView)` — L1317. src = msaa image,
  dst = 1x image, same dimensions/format.
- `DkMultisampleState` struct + `dkCmdBufBindMultisampleState` (L1269):
  `mode`/`rasterizerMode` = the DkMsMode used at draw time; grep
  `dkMultisampleStateDefaults` for the initializer.
- Descriptor note (T34 will rely on it): `dkImageDescriptorInitialize`
  (L1346) takes `decayMS` — the 1x resolve target is the texture anyone samples.

## Steps

1. Extend `dkrt::Desc` with `uint32_t samples = 1;` (maps 1/2/4/8 → DkMsMode;
   reject anything else with one `[dk] rt bad sample count N` line, return false).
2. `FullTarget` gains the optional resolve half: when `samples > 1`, create the
   1x sibling image (own memblock, `UsageRender` so it can also be blitted),
   and a `resolve()` that records `dkCmdBufResolveImage` on the current cmd buf.
   Add `DkImageView* resolved_view()` for later consumers (T31/T38).
3. `bind()` on an msaa target binds the MSAA image (draws go there); document
   that the consumer must `resolve()` before sampling from `resolved_view()`.
4. `matches(w,h,msaa)` now takes the sample count (GL Fbo::matches semantics).

## Rules

- Only `dk_render_target.h/.cpp`. Every create checked; one-line `[dk]` failures;
  teardown covers all three memblocks. Never crash.
- Do NOT wire msaa_samples from Gfx settings anywhere — T38/T39 decide policy.

## Done-check

```sh
scripts/ai/check-bg.sh jak1 && scripts/ai/check-tail.sh
```

DONE-PASS + NRO md5 unchanged (`08fc9510…`). Changed → not inert, stop.

## Doubts

- If Example5 shows an extra barrier around resolve (2D/3D engine mixing), copy
  its exact barrier usage and note it in the header comment.
