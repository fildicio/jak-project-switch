# T23 — TexturePool seam: GOAL texture path → dk behind the kill-switch (P2.3)

**REWRITTEN 2026-10-17 16:45 (reviewer) after the first session stalled 2h51m in a
deliberation loop** — it re-decided the same design questions for hours and never
reached the Done-check. Every question it agonized over is now DECIDED below with
file:line evidence. Do not re-derive, compare alternatives, or second-guess: verify
the two facts you need at each edit point, then execute.

**Tree state:** that session's first edit is ALREADY ON DISK and correct (see
"Already on disk"). Verify it, keep it, build on top of it.

## Read first (exactly these, nothing else)
1. This whole file.
2. `game/graphics/texture/TexturePool.cpp` L16–L85 (landed includes + `upload_to_gpu`)
   and L354–L370 (the placeholder caller).
3. `game/graphics/texture/TexturePool.h` L133–L160 (`TextureData`, `GpuTexture`) and
   L360–L366 (`m_placeholder_data`, `m_placeholder_texture_id`).
4. `game/graphics/deko3d_renderer/dk_texture_gpu.h` L35–L102 — the ONLY dktexgpu API
   you may call: `upload_rgba8(w, h, src)` and `active()`. Nothing else from it.

## FACTS (measured — rediscovering these cost the last session 3 hours)
- The ONLY `glTexImage2D` in `TexturePool.cpp` is inside `upload_to_gpu` (.cpp:68–85).
  There is no other GL upload in this file to seam.
- `u64 upload_to_gpu(const u8*, u16, u16)` is a free function; its ONLY caller today is
  the placeholder init at .cpp:368 (`m_placeholder_texture_id = upload_to_gpu(...)`).
- Real/level textures arrive PRE-UPLOADED as `TextureInput.gpu_texture` (GLuint,
  .h:203) from code outside this file (LoaderStages / `opengl_renderer/` — untouchable,
  P3's problem). `give_texture` (.cpp:87) does NOT upload. There is no
  `update_gl_texture` and no `TextureReference` logic in this file.
- `dktexgpu::upload_rgba8(uint32_t w, uint32_t h, const void* src)` returns
  `uint32_t`: id ≥ 2 on success, **0 = "skip this texture"** (FIX 33 caller
  contract). dk_texture_gpu.h L24–27: a 0-return failure is ALREADY LOGGED at the
  source — the caller does NOT log again.
- `dkdev::is_up()` is declared in `game/graphics/pipelines/dk_device.h` (already
  included by the on-disk edit below).

## DECISIONS (final — implementing them exactly is the whole task)
- **D1** — The seam is ONE branch at the top of `upload_to_gpu`, inside
  `#if defined(__SWITCH__)`, so the desktop build stays byte-identical.
- **D2** — No caller-side `switch_run_logf` for the 0-return (source logs it). The
  hand-declared `switch_run_logf` (.cpp:29–34, already on disk) STAYS: an unused
  extern declaration, kept deliberately for P3 logging hooks. Do not remove it, do
  not add calls to it.
- **D3** — `GpuTexture` gets `u64 dk_id = 0;` inside `#if defined(__SWITCH__)`. It
  stays 0 for now — no real texture has a dktex id yet. That is intended; say so in
  the report in one line.
- **D4** — NO `add_level_ref` / `release` wiring in this task: those track dktex ids,
  and no id reaches `give_texture` bookkeeping yet (see FACTS). The P3 mapping is
  documented in the seam comment (D5) instead.
- **D5** — The required deferral comment goes INSIDE the seam branch: level-texture
  rerouting, the add_level_ref/release level-ref mapping, and the LoaderStages BCn
  path (`upload_bcn`) are P3 on the dk path.

## Already on disk — VERIFY, KEEP (do not rewrite)
- `TexturePool.cpp` L22–27: `#if defined(__SWITCH__)` include block
  (`dk_texture_gpu.h`, `dk_device.h`) + comment. Correct as written.
- `TexturePool.cpp` L29–34: hand-declared `switch_run_logf` + comment. Correct.

## Edits

### `game/graphics/texture/TexturePool.h`
E1. In `struct GpuTexture` (starts .h:142), after the LAST existing member, before the
    closing brace, add EXACTLY:
```cpp
#if defined(__SWITCH__)
  // T23 (AI-assisted): dktex::TextureStore id of this texture's deko3d copy
  // (0 = none). Only the placeholder path can set it today; real/level textures
  // get their ids in P3 when LoaderStages reroutes through dktexgpu.
  u64 dk_id = 0;
#endif
```
Read the struct to its end first; touch nothing else in it.

### `game/graphics/texture/TexturePool.cpp`
E2. In `upload_to_gpu` (.cpp:68), as the FIRST statements of the function body:
```cpp
#if defined(__SWITCH__)
  if (dkdev::is_up() && dktexgpu::active()) {
    // T23 (AI-assisted): deko3d seam. upload_rgba8 returns a store id (>= 2) or 0;
    // 0 means "skip this texture" (FIX 33 caller contract) and the failure is
    // already logged at the source (dk_texture_gpu.h caller contract) — no second
    // log here. P3 on the dk path, explicitly NOT this task: (a) rerouting real /
    // level textures (they arrive pre-uploaded via TextureInput from LoaderStages /
    // opengl_renderer), (b) the add_level_ref / release level-ref mapping in
    // give_texture bookkeeping, (c) the BCn path (dktexgpu::upload_bcn).
    uint32_t dk_tex = dktexgpu::upload_rgba8(w, h, data);
    return dk_tex;  // 0 propagates "no texture" to the caller, like a GL failure.
  }
#endif
```
E3. NOTHING else in this file: the GL body, placeholder init (.cpp:354–370),
    `give_texture`, and every other function stay untouched.

## Done-check (background wrappers ONLY)
```
scripts/ai/check-bg.sh jak1
scripts/ai/check-tail.sh
```
QWEN_REPORT `## T23 – PASS`: files changed; GL→dk mapping table (2 rows:
`glGenTextures`+`glTexImage2D` → `dktexgpu::upload_rgba8`, `tex_id` → store id);
last 5 check lines + md5; grep output proving every edit sits inside `__SWITCH__`
guards; one line stating D3/D4 (`dk_id` stays 0 today, ref-count mapping deferred to
P3 by design). Flip the README T23 row to 🟡. No commit, no deploy.

## STOP rule (unchanged)
On any command failure: append ONLY the last 5 lines to the report, no self-fixing,
report STOP. If an API name in this file does not exist exactly as written: STOP and
report — do not guess a substitute.
