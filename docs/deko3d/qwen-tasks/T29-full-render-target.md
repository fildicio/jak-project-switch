# T29 — ✂️ SPLIT 2026-10-18 — superseded by T29a + T29b (do NOT run this file)

> The coder looped 100 minutes on this task with no output: the open-ended Fbo
> inventory and the dkrt FullTarget code + build do not fit one session. The
> inventory is now **T29a** (doc-only, time-boxed) and the code is **T29b**.
> Original task text kept below for history.

Track D P3.1 · Depends on: T28 ✅ · Inert · One session (original header)

## Goal

1. Add `dkrt::FullTarget` to dk_render_target: color+depth created/matched/
   torn-down as ONE unit with a `matches(w,h,msaa)` rule copied from the GL
   `Fbo::matches` (Fbo.h:23-30) — this is what a P3 renderer re-creates on
   resolution change instead of 9 ad-hoc GL FBOs.
2. Record (comment table in the .h) which GL `Fbo` instances exist, so later
   renderers know what they must reproduce. Step 0 grounds it — do not guess.

## Context

- `game/graphics/opengl_renderer/OpenGLRenderer.h` — `FboState` at L154-161
  (`window`, `render_buffer`, `resolve_buffer`, `render_fbo`), `m_fsr_mid` L150.
  Run `grep -n 'Fbo' game/graphics/opengl_renderer/OpenGLRenderer.h` and read every
  member; then `grep -rn 'Fbo ' game/graphics/opengl_renderer/*.h | grep -v OpenGLRenderer.h`
  for strays (Sky, DepthCue, etc. may own one). Write the list into the header
  comment as `file:line — purpose`.
- `docs/deko3d/gl-inventory.md` — `glBindFramebuffer` appears 62 times total; you
  are only enumerating the OWNERS of Fbos, not every bind.

## API cheat-sheet (all verified; no new deko3d calls)

- Nothing new vs T27/T28: `FullTarget` is bookkeeping over the existing
  `init/Desc{with_depth=true}/bind/teardown/valid` pair.
- `matches(int w, int h, int msaa)` semantics copied from GL: effective msaa =
  `multisampled ? multisample_count : 1`.

## Steps

1. In `dk_render_target.h` add:
   ```cpp
   struct FullTargetDesc { uint32_t width, height; bool with_depth; };
   struct FullTarget {
     bool init(const FullTargetDesc& d);   // color (with optional depth) in one call
     bool matches(uint32_t w, uint32_t h) const;
     void bind();                          // binds color (+depth when present)
     void teardown();
     bool valid() const;
     uint32_t width() const; uint32_t height() const;
   };
   ```
   MSAA member comes in T30 — leave it out now, keep `matches` 2-arg.
2. Implement in the .cpp over the existing dkrt state (a FullTarget OWNS one
   color Desc + the T28 depth half). One `FullTarget` instance = one pair of
   memblocks; dkrt may host several.
3. `bind()` must first call the existing color `bind()` path so the T28 depth view
   is included; do not duplicate `dkCmdBufBindRenderTarget` code — factor a private
   `bind_views()` used by both.
4. Write the Fbo inventory comment (step 0 results) at the top of the .h.

## Rules

- Only `dk_render_target.h` / `dk_render_target.cpp`.
- Plain header rule (no `<deko3d.h>`), `.cpp` inside `#if defined(__SWITCH__)`.

## Done-check

```sh
scripts/ai/check-bg.sh jak1 && scripts/ai/check-tail.sh
```

DONE-PASS + NRO md5 unchanged (`74635895…`). If changed → not inert, stop.

## Doubts

- If the Fbo inventory step finds Fbos in files the plan's "9 framebuffers"
  count does not match, list them — do not force the count.
