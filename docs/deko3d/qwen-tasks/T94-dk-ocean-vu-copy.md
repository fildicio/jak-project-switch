# T94 — Ocean dk VU copies: Near + Mid (compile-only, diff-gated)

**Depends on:** T93. **Category:** D ocean. **Size:** medium (1 session, mostly paste).
**Law:** zero design freedom. Everything is a verbatim copy with a rename table. The
result must COMPILE and contain no GL — it is not wired into anything yet (drivers,
recorder use, bucket wiring are later reviewer tasks). Anything not in a table → STOP,
paste the problem in the report, change nothing else.

## Goal
The two `_PS2` files (9,223 LOC, T91c: pure VU1 math, `gl[A-Z]` = 0) are the bulk of the
ocean. Per repo law (never edit `opengl_renderer/` — copy and adapt) they are copied into
`game/graphics/deko3d_renderer/` with the GL-typed members swapped for the T93 recorder.

## Create 1 — `game/graphics/deko3d_renderer/dk_ocean_near.h`
Copy `game/graphics/opengl_renderer/ocean/OceanNear.h` and apply:

| old (OceanNear.h) | new (dk_ocean_near.h) |
|---|---|
| first line `#pragma once` | keep, then paste the header comment below |
| `#include "game/graphics/opengl_renderer/BucketRenderer.h"` | DELETE |
| `#include "game/graphics/opengl_renderer/ocean/CommonOceanRenderer.h"` | `#include "game/graphics/deko3d_renderer/dk_ocean_common.h"` |
| `#include "game/graphics/opengl_renderer/ocean/OceanTexture.h"` | DELETE |
| `class OceanNear : public BucketRenderer {` | `namespace dkocean {` + blank line + `class Near {` |
| public block lines: `OceanNear(const std::string& name, int my_id);`, `render(...) override;`, `render_jak1(...)`, `render_jak2(...)`, `draw_debug_window() override;`, `init_textures(...) override;` | DELETE all six (the driver-half task adds `run()` back) |
| `  OceanTexture m_texture_renderer;` | DELETE |
| `  CommonOceanRenderer m_common_ocean_renderer;` | `  CommonRecorder m_recorder;` |
| everything else (all VU structs, buffers, helpers, `xgkick` decl) | VERBATIM |
| last line `};` | `};` + blank + `}  // namespace dkocean` |

Header comment to paste after `#pragma once`:
```cpp
// T94 (AI-assisted): deko3d ocean port, near-ocean VU1 math. Verbatim copy of GL
// OceanNear (the _PS2 VU emulation is pure CPU, T91c) with the GL-typed members swapped
// for dkocean::CommonRecorder (T93). The DMA driver loop (run/render_jak1/render_jak2)
// is a later task. Plain C++ header (no deko3d types).
```

## Create 2 — `game/graphics/deko3d_renderer/dk_ocean_near.cpp`
Copy `game/graphics/opengl_renderer/ocean/OceanNear_PS2.cpp` (the WHOLE file, comment
lines included) and apply:

| old | new |
|---|---|
| `#include "OceanNear.h"` | the skeleton below |
| `OceanNear::` | `Near::` |

Skeleton (paste the copied bodies where marked):
```cpp
// T94 (AI-assisted): see dk_ocean_near.h. Verbatim copy of GL OceanNear_PS2.cpp
// (rename table in the task file) + xgkick from OceanNear.cpp:221.
#include "game/graphics/deko3d_renderer/dk_ocean_near.h"

#if defined(__SWITCH__) || defined(DK_HOST_REPLAY)

namespace dkocean {

// ---- copied _PS2 bodies here ----

// from OceanNear.cpp:221 (m_common_ocean_renderer -> m_recorder)
void Near::xgkick(u16 addr) {
  m_recorder.kick_from_near((const u8*)&m_vu_data[addr]);
}

}  // namespace dkocean
#endif
```

## Create 3 — `game/graphics/deko3d_renderer/dk_ocean_mid.h`
Copy `game/graphics/opengl_renderer/ocean/OceanMid.h` and apply:

| old (OceanMid.h) | new (dk_ocean_mid.h) |
|---|---|
| first line `#pragma once` | keep, then paste the header comment below |
| `#include "game/graphics/opengl_renderer/BucketRenderer.h"` | DELETE |
| `#include "game/graphics/opengl_renderer/DirectRenderer.h"` | `#include "game/graphics/opengl_renderer/AdgifHandler.h"` (AdGifData source, SkyDk.cpp precedent) |
| `#include "game/graphics/opengl_renderer/ocean/CommonOceanRenderer.h"` | `#include "game/graphics/deko3d_renderer/dk_ocean_common.h"` |
| `class OceanMid {` | `namespace dkocean {` + blank + `class Mid {` |
| public decls `OceanMid();`, `run(...)`, `run_jak2(...)` | DELETE (driver-half task adds them back) |
| `  CommonOceanRenderer m_common_ocean_renderer;` | `  CommonRecorder m_recorder;` |
| everything else (Constants struct, Vu1Data, m_vu_data, all helpers) | VERBATIM |
| last line `};` | `};` + blank + `}  // namespace dkocean` |

Header comment:
```cpp
// T94 (AI-assisted): deko3d ocean port, mid-ocean VU1 math. Verbatim copy of GL
// OceanMid (the _PS2 VU emulation is pure CPU, T91c) with the GL-typed member swapped
// for dkocean::CommonRecorder (T93). The DMA driver loop (run/run_jak2) is a later
// task. Plain C++ header (no deko3d types).
```

## Create 4 — `game/graphics/deko3d_renderer/dk_ocean_mid.cpp`
Copy `game/graphics/opengl_renderer/ocean/OceanMid_PS2.cpp` (WHOLE file, comments
included) and apply:

| old | new |
|---|---|
| `#include "OceanMid.h"` | the skeleton below |
| `OceanMid::` | `Mid::` |

Skeleton: same shape as `dk_ocean_near.cpp` (guard, `namespace dkocean`, copied bodies,
then):
```cpp
// from OceanMid.cpp:261 (m_common_ocean_renderer -> m_recorder)
void Mid::xgkick(u16 addr) {
  m_recorder.kick_from_mid((const u8*)&m_vu_data[addr]);
}
```

## Edit (1 line)

`game/CMakeLists.txt` — add `graphics/deko3d_renderer/dk_ocean_near.cpp` and
`graphics/deko3d_renderer/dk_ocean_mid.cpp` directly under the
`graphics/deko3d_renderer/dk_ocean_common.cpp` line added by T93.

## Done-check (all must pass)

```
python3 scripts/ai/check-dk.py
cmake --build build-host --target goalc-test -j8 2>&1 | tail -3
grep -n "gl[A-Z]" game/graphics/deko3d_renderer/dk_ocean_near.h game/graphics/deko3d_renderer/dk_ocean_near.cpp game/graphics/deko3d_renderer/dk_ocean_mid.h game/graphics/deko3d_renderer/dk_ocean_mid.cpp ; echo "gl-grep exit: $?"
```

- dk-lint: PASS
- host build: compiles + links (no tests yet — these classes are not called)
- gl-grep: no matches (exit 1)
- NOTHING is wired into `dk_renderer.cpp` / any bucket in this task.

## Report (QWEN_REPORT.md §T94)

1. The three done-check outputs.
2. FULL output of
   `diff -u game/graphics/opengl_renderer/ocean/OceanNear_PS2.cpp game/graphics/deko3d_renderer/dk_ocean_near.cpp`
   and the same for `OceanMid_PS2.cpp` / `dk_ocean_mid.cpp` — paste both diffs whole;
   the reviewer verifies every hunk is rename-table-only (expected: include line,
   `OceanNear::`/`OceanMid::` → `Near::`/`Mid::`, namespace/guard wrapper, appended
   xgkick).
