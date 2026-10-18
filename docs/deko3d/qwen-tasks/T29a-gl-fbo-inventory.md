# T29a — GL Fbo inventory doc (P3.1 slice 3a, research only, AI-assisted)

Track D P3.1 · Depends on: T28 (committed, verified — no code needed from it) ·
**DOC-ONLY: zero code files touched, no build, no hardware** · One SHORT session
(time box ~30 min MAX)

> Split out of T29 on 2026-10-18 after the coder looped 100 minutes with no output.
> This half is RESEARCH ONLY: read the files listed below, write ONE doc, done.
> If you feel the urge to open `OpenGLRenderer.cpp` whole — DON'T (it is ~5,000
> lines). Every read below is a `grep` plus ±10 lines around each hit.

## Deliverable — exactly ONE new file

`docs/deko3d/gl-fbo-inventory.md` — a table of every GL framebuffer ("Fbo") OWNER in
the desktop renderer, so T29b/T30/T31/T32 know what dkrt must reproduce (the plan's
P3.1 "9 framebuffers, 7 blits, 1 readpixels" inventory).

One table row per owner: `file:line — member/variable — purpose — size/format (if
fixed) — msaa? — used as blit src/dst?`. End the doc with (a) the TOTAL owner count
(if it is not 9, say so — do not force the count) and (b) one short paragraph
"what dkrt must cover" (which owners are resolution-following vs fixed-size, which
are blit sources/destinations).

## Read EXACTLY these (locations pre-verified — do not re-find them)

1. `game/graphics/opengl_renderer/Fbo.h` — the Fbo class itself, whole file (small),
   note `matches()` L23-30.
2. `game/graphics/opengl_renderer/OpenGLRenderer.h` — run
   `grep -n 'Fbo' game/graphics/opengl_renderer/OpenGLRenderer.h` and read ONLY
   ±3 lines per hit (`FboState` L154-161: `window`, `render_buffer`,
   `resolve_buffer`, `render_fbo`; `m_fsr_mid` L150). Do NOT read the whole file.
3. These 7 files are the ONLY places `glGenFramebuffers` appears under game/graphics
   (reviewer-verified). Re-run once to confirm —
   `grep -rln glGenFramebuffers game/graphics --include='*.cpp' --include='*.h' | grep -v build`
   — if your list differs, use yours and say so in the report:
   - `game/graphics/opengl_renderer/OpenGLRenderer.cpp`
   - `game/graphics/opengl_renderer/opengl_utils.cpp`
   - `game/graphics/opengl_renderer/SkyBlendGPU.cpp`
   - `game/graphics/opengl_renderer/DepthCue.cpp`
   - `game/graphics/opengl_renderer/sprite/Sprite3_Distort.cpp`
   - `game/graphics/opengl_renderer/sprite/GlowRenderer.cpp`
   - `game/graphics/opengl_renderer/gl_time_split_funcs.h`
   For each: `grep -n 'glGenFramebuffers\|Fbo ' <file>` then read ±10 lines per hit.
   Never open a whole .cpp.
4. `docs/deko3d/gl-inventory.md` — cross-check counts only. `glBindFramebuffer`
   appears 62 times total there; you are enumerating OWNERS, not every bind.

## Rules

- You create ONE .md file. You do NOT modify any code, header, CMake, or other task
  file.
- TIME BOX: this is a 20–30 min reading task. If you are still grepping after
  ~15 min, write the partial table (mark unreached rows `TODO — not reached`) and
  STOP. A partial honest table beats a loop.

## Done-check (no build — a .md cannot change the NRO)

```sh
git status --short   # must show ONLY: ?? docs/deko3d/gl-fbo-inventory.md
```
If ANY code file shows up as modified, restore it (`git checkout -- <file>`) and say
so in your report. Report `## T29a`: paste the full table, the total count, and the
"what dkrt must cover" paragraph. Flip the README T29a row to 🟡.

## Doubts

- If a hit is ambiguous (e.g. an Fbo created but apparently never bound), mark that
  row `unclear — reviewer` and move on. Do not chase it.
