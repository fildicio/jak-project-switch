# T29b — dkrt FullTarget (color+depth pair) (P3.1 slice 3b, code, AI-assisted)

Track D P3.1 · Depends on: T29a ✅ (needs its doc) + T28 (committed, verified) ·
Inert · One session

> The code half of the old T29 (split 2026-10-18 after a 100-min no-output coder
> loop). The Fbo-inventory reading moved to T29a — do NOT re-do it here; T29a's
> output is at `docs/deko3d/gl-fbo-inventory.md`.

## Goal

`dkrt::FullTarget` — color+depth created/matched/torn-down as ONE unit, with a
`matches(w,h)` rule copied from the GL `Fbo::matches` (Fbo.h:23-30). This is what a
P3 renderer re-creates on resolution change instead of 9 ad-hoc GL FBOs.

## Read first (exactly these)

1. `game/graphics/deko3d_renderer/dk_render_target.h` + `.cpp` — the T27+T28 state
   (Desc/`with_depth`, colour + Z24S8 memblocks, init/bind/teardown/valid).
2. `game/graphics/opengl_renderer/Fbo.h` L23-30 — `matches()` semantics
   (`multisampled ? multisample_count : 1`; MSAA itself is T30 — keep `matches`
   2-arg for now).
3. `docs/deko3d/gl-fbo-inventory.md` (T29a's output) — you will paste its owner
   table as a comment at the top of dk_render_target.h (old T29 step 4).

## API cheat-sheet (verified; no new deko3d calls)

- Nothing new vs T27/T28: `FullTarget` is bookkeeping over the existing
  `init / Desc{with_depth=true} / bind / teardown / valid` pair.

## Steps

1. In `dk_render_target.h` add:
   ```cpp
   struct FullTargetDesc { uint32_t width, height; bool with_depth; };
   struct FullTarget {
     bool init(const FullTargetDesc& d);   // colour (with optional depth) in one call
     bool matches(uint32_t w, uint32_t h) const;
     void bind();                          // binds colour (+depth when present)
     void teardown();
     bool valid() const;
     uint32_t width() const; uint32_t height() const;
   };
   ```
   MSAA member comes in T30 — leave it out now, keep `matches` 2-arg.
2. Implement in the .cpp over the existing dkrt state (a FullTarget OWNS one colour
   Desc + the T28 depth half). One `FullTarget` instance = one pair of memblocks;
   dkrt may host several.
3. `bind()` must go through the existing colour `bind()` path so the T28 depth view
   is included; do not duplicate `dkCmdBufBindRenderTarget` code — factor a private
   `bind_views()` used by both.
4. Paste the T29a inventory table as a comment at the top of the .h.

## Rules

- Only `dk_render_target.h` / `dk_render_target.cpp`.
- Plain header rule (no `<deko3d.h>`), .cpp inside `#if defined(__SWITCH__)`.
- FullTarget is inert: nothing calls it yet. Do NOT wire it into any renderer.

## Done-check

```sh
scripts/ai/check-bg.sh jak1 && scripts/ai/check-tail.sh
```

DONE-PASS + NRO md5 unchanged (`08fc9510…`). If changed → not inert, stop.

(Reviewer 2026-10-18: the original expected md5 here was `74635895…`; it no
longer reproduces on ANY tree because the devkitPro devkita64 image was
re-pulled (2nd drift: 727feb1c→74635895→08fc9510). T29b's own build, a fully
reverted clean-HEAD build, and the reviewer's re-run are all byte-identical at
`08fc9510745f7c584ab6c8bd73a1b883` → T29b is inert. Baseline re-baselined.)

## Doubts

- If T29a's table and the plan's "9 framebuffers" count disagree, list the
  discrepancy in your report — do not force the count.
