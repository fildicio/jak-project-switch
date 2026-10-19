# T30a — dkrt MSAA image creation (P3.1 slice 4a, AI-assisted)

Track D P3.1 · Depends on: T29b ✅ · Inert · One session (~40 min)
Split from T30 (2026-10-18: the whole-task session died reading reference files).

## Goal

Half 1 of T30. Give `dkrt::FullTarget` a multisampled COLOUR image: validate
`samples`, create the image as 2DMS when samples > 1, extend `matches()` with
the sample count. **NO resolve sibling, NO resolve() call — that is T30b.**

## Session rules (hard — read twice)

- Time box ~40 min. Over budget → STOP at the last clean compile; report what remains.
- Read each listed range ONCE. Catch yourself re-reading → you are looping → STOP, report.
- FORBIDDEN reads: `deko3d.h`/`deko3d.hpp`, everything under `docs/deko3d-ref/`,
  `SWITCH_PORT_SESSION_NOTES.md`, `DEKO3D_MIGRATION_PLAN.md`. Every API line you
  need is pasted below.
- Build command is exactly `scripts/ai/check-bg.sh jak1` — jak1, never jak2.
- Report goes in QWEN_REPORT.md under `## T30a`.

## Current state (verified — do not re-derive)

- `dk_render_target.h` ALREADY has `uint32_t samples = 1;` in `Desc` (L72) and
  `FullTargetDesc` (L121) — the aborted session added them. KEEP them.
- `FullTarget::matches(uint32_t w, uint32_t h) const` is declared at .h L149.
- The aborted session changed ONLY the header; the .cpp is untouched T29b state.

## Read (once, exactly this)

1. `grep -n 'FullTarget::' game/graphics/deko3d_renderer/dk_render_target.cpp`
   then read ONLY the bodies of `FullTarget::init` and `FullTarget::matches`.

## API (pasted verbatim from deko3d.h — do not open it)

```c
DkImageType_2DMS = 6,   // L359 — image type for multisampled 2D
DkMsMode_1x = 0,        // L530-533
DkMsMode_2x = 1,
DkMsMode_4x = 2,
DkMsMode_8x = 3,
// DkImageLayoutMaker has the field:   DkMsMode msMode;        (L558)
// dkImageLayoutMakerDefaults sets:    maker->msMode = DkMsMode_1x;   (L574)
```

Exact sample→mode map: 1→`DkMsMode_1x`, 2→`DkMsMode_2x`, 4→`DkMsMode_4x`,
8→`DkMsMode_8x`. Any other value: one line `[dk] rt bad sample count N`, init
returns false.

## Steps

1. `FullTarget::init`: validate `d.samples` first. When samples > 1, set the
   colour image layout `type = DkImageType_2DMS` and `msMode` = mapped mode.
   Depth image stays 1x (comment why: multisampled depth is out of scope).
   Everything else in init is unchanged.
2. `matches(w, h)` → `matches(uint32_t w, uint32_t h, uint32_t msaa) const`
   (GL `Fbo::matches` semantics). Update in-tree callers of the old signature —
   there should be NONE outside dkrt itself; if you find one, STOP and report.
3. `bind()`: add ONE comment line — draws land in the MSAA image; the consumer
   must resolve (T30b) before sampling. No code change.

## Rules

- Only `dk_render_target.h/.cpp`. Checked creates, one-line `[dk]` failures,
  never crash. No new callers (stays inert).

## Done-check

```sh
scripts/ai/check-bg.sh jak1 && scripts/ai/check-tail.sh
```

DONE-PASS + NRO md5 `08fc9510745f7c584ab6c8bd73a1b883` UNCHANGED (uncalled code
gc-strips — the T29b precedent). Changed → not inert → STOP.

## Doubts

- If init's image creation cannot take `type`/`msMode` without restructuring
  more than ~15 lines → STOP and report; do not restructure.
