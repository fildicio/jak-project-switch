# T43 — Soak fix: level textures → dktexgpu (`add_texture` seam, P2.3 slice 1, AI-assisted)

Track D P2.3 · Depends on: T23 ✅ (seam pattern), T24 ✅ + the 2026-10-17 soak finding ·
**First LIVE change since T24 — the NRO md5 WILL change; that is expected, do not stop.**
Run AFTER the T29a–T37 inert batch · One session

## Why (the 2026-10-17 2-min hardware soak, T24 protocol, failed)

Deployed the T24-era NRO, ran 2 minutes, entered jungle:

- no `[dk] tex id=…` upload line ever appeared; telemetry `[dk] tex live=0` — ZERO
  textures reached dktexgpu (`fail=0`: it was never called, not called-and-failing).
- `[loader] live=… init=1 …` with prefetch blocked by "a level is still staging"
  forever — `m_initializing_tfrag3_levels` never drains (Loader.cpp:317).
- `[loader] … pool=… bufs 0.0MB free, 100 out, …` (Loader.cpp:1945).

Root cause (verified in code): on the dk path (`gk_dk3d.txt` present) there is NO
OpenGL context, but `LoaderStages.cpp` still runs the entire GL staging pipeline.
`add_texture` (LoaderStages.cpp:949) does `glGenTextures` +
`glTexImage2D`/`glCompressedTexImage2D` for every level texture — silent no-ops with
no context. The T23 seam only covers `TexturePool::upload_to_gpu`, whose ONLY caller
is the placeholder init — level textures never touch it. This task is the plan's
P2.3 "Loader/LoaderStages integration" first slice (DEKO3D_MIGRATION_PLAN.md, §P2.3).

## Scope (textures ONLY — do not creep)

- IN: RGBA level/common textures uploaded through `dktexgpu::upload_rgba8`, with the
  existing `pool.give_texture` bookkeeping preserved.
- OUT (one report line each, NO code): geometry/merc/collide buffers (`GpuBufferPool`
  stays GL — later P2.3 slice); BCn textures (jak2/3 fr3) skipped with a one-shot log;
  mip chains (level 0 only, exactly like the GL FIX 42/48 path); drawing these
  textures (P3.2, T36/T38).

## Read first (exactly these)

1. This whole file.
2. `game/graphics/opengl_renderer/loader/LoaderStages.cpp` L940–1060 — `add_texture`:
   the BCn branch, the FIX 48/69 RGBA branch, and both `pool.give_texture` blocks.
   Your seam point.
3. `game/graphics/texture/TexturePool.cpp` L16–34 and L69–90 — the T23 seam: copy this
   exact `#ifdef __SWITCH__` include pattern and the `dkdev::is_up() &&
   dktexgpu::active()` gate.
4. `game/graphics/deko3d_renderer/dk_texture_gpu.h` L57–71 — `upload_rgba8(w, h, src)`:
   id ≥ 2 on success; **0 = "skip this texture", the failure is ALREADY logged at the
   source — do NOT log again** (FIX 33 caller contract).

## Facts (verified — do not re-derive)

- Both call sites of `add_texture` (L1171 and L1259) read
  `ld.textures.push_back(add_texture(*data.tex_pool, tex, false))` — seaming the one
  function covers both.
- `tfrag3::Texture` fields already used right there (L949–1050): `w`, `h`, `data`
  (RGBA bytes, level 0), `format` (`tfrag3::TEXTURE_FMT_RGBA`), `load_to_pool`,
  `combo_id`, `debug_name`, `debug_tpage_name`. Use only these.
- `TextureInput` fields (used at L982–990): `debug_page_name`, `debug_name`, `w`,
  `h`, `gpu_texture`, `common`, `id`, `src_data`.
- LoaderStages.cpp has NO dk includes yet, and its logs use `fmt::print`
  (e.g. L1013 `[loader] FIX 48 texture path: …`, guarded by a `static bool`).
- Render-thread only: GPU loader stages (and `add_texture`) run on the render thread,
  which is exactly dktexgpu's thread contract (dk_texture_gpu.h L29).

## Edits — ONLY `game/graphics/opengl_renderer/loader/LoaderStages.cpp`

E1. After `#include "Loader.h"` (L14), add the guarded include block (same shape as
TexturePool.cpp L22–27):

```cpp
#ifdef __SWITCH__
// T43 (AI-assisted): level-texture seam. On the deko3d path there is no GL
// context, so add_texture() must hand level textures to dktexgpu instead of GL.
#include "game/graphics/pipelines/dk_device.h"             // dkdev::is_up()
#include "game/graphics/deko3d_renderer/dk_texture_gpu.h"  // dktexgpu::upload_rgba8
#endif
```

E2. As the FIRST statements of `add_texture`'s body (before `GLuint gl_tex;`), add:

```cpp
#ifdef __SWITCH__
  // T43 (AI-assisted): deko3d seam — same gate as the T23 TexturePool seam. With no
  // GL context on the dk path the glGenTextures/glTexImage2D below are silent
  // no-ops, so level textures never reach the GPU and the level stalls in staging.
  if (dkdev::is_up() && dktexgpu::active()) {
    static bool s_reported = false;
    if (!s_reported) {
      s_reported = true;
      fmt::print("[dk] loader texture path: dktexgpu::upload_rgba8 (no GL)\n");
    }
    uint32_t dk_tex = 0;
    if (tex.format == tfrag3::TEXTURE_FMT_RGBA) {
      // Single mip (level 0), like the GL FIX 42/48 path. A 0 return means
      // "skip" and is already logged inside dktexgpu — do not log again.
      dk_tex = dktexgpu::upload_rgba8(tex.w, tex.h, tex.data.data());
    } else {
      static bool s_bcn_reported = false;
      if (!s_bcn_reported) {
        s_bcn_reported = true;
        fmt::print("[dk] loader tex: BCn deferred (later P2.3 slice), skipping\n");
      }
    }
    if (tex.load_to_pool) {
      // Bookkeeping only, mirroring the GL branches below. gpu_texture = 0: there
      // is no GL name on this path; the real dktex-id mapping is T36/P3.2 work.
      // src_data keeps SkyBlendCPU's CPU-pixel reader working (RGBA only).
      TextureInput in;
      in.debug_page_name = tex.debug_tpage_name;
      in.debug_name = tex.debug_name;
      in.w = tex.w;
      in.h = tex.h;
      in.gpu_texture = 0;
      in.common = is_common;
      in.id = PcTextureId::from_combo_id(tex.combo_id);
      in.src_data =
          (tex.format == tfrag3::TEXTURE_FMT_RGBA) ? (const u8*)tex.data.data() : nullptr;
      pool.give_texture(in);
    }
    return dk_tex;  // 0 propagates "skipped" to the caller, like a GL failure
  }
#endif
```

E3. NOTHING else: the GL body below the seam, `upload_bcn_texture`, every other
function, and all FIX/budget logic stay untouched. The desktop path must stay
byte-identical — everything you add is inside `__SWITCH__` guards.

## Rules

- Only LoaderStages.cpp; every added line inside `#ifdef __SWITCH__`.
- This TU lives under `opengl_renderer/`: you are extending an EXISTING
  `#ifdef __SWITCH__` FIX-block pattern (like FIX 48/69/74), not porting renderer
  code. Do not restructure, rename, or "clean up" anything.
- No new `std::thread`. No deko3d header except the two plain dk headers in E1 —
  LoaderStages.cpp must NOT include `<deko3d.h>`.
- Logs: one-shot only (`static bool` guard), one line, `[dk]`/`[loader]` prefix,
  same style as the FIX 48 one-shot at L1010–1013.

## Done-check (background wrappers ONLY)

```sh
scripts/ai/check-bg.sh jak1 && scripts/ai/check-tail.sh   # repeat until DONE-PASS / DONE-FAIL
```

Expected: **DONE-PASS** and the NRO md5 **DIFFERENT from `08fc9510…`** — that is
correct for a LIVE change; paste the new md5 in your report and do NOT stop.
Report `## T43`: files changed, the new md5, `git diff --stat`, the exact new log
strings, and one line per OUT-of-scope item above. Flip the README T43 row to 🟡.
No commit, no deploy.

After review the USER re-runs the jungle soak: expect the one-shot
`[dk] loader texture path` line, `[dk] tex id=…` lines during the load,
`[dk] tex live` growing, then `[loader] level jungle ready in …`. If jungle still
stalls on "a level is still staging", the geometry `GpuBufferPool` slice is next —
report which `[loader]` fields froze.

## Doubts

- If `tfrag3::TEXTURE_FMT_RGBA`, `PcTextureId::from_combo_id`, or any field name in
  E2 does not compile exactly as written: STOP and report — do not guess a substitute.
- If `pool.give_texture` with `gpu_texture = 0` trips an assert in a host test,
  STOP and report — do not paper over it.
