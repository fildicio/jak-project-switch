# T26 — dkpipe::apply_blend_const: FIX64_SUB blend const at bind time (P3.0)

Tiny P3.0 task, independent of T25. Closes the gap documented at
`game/graphics/deko3d_renderer/dk_state.cpp:76–82`: `Blend::Fix64Sub` uses
`DkBlendFactor_ConstColor` on both sides, but `DkBlendState` has no blend-color field —
the constant must be emitted on the command buffer when a pipeline is bound. **Inert**:
no bind path exists until P3.2.

## Verified anchors
- `dkCmdBufSetBlendConst(DkCmdBuf, float r, g, b, a)` — deko3d.h:1293.
- The Fix64Sub case (glBlendColor(0.5,...) on GL) — dk_state.cpp:71–82.
- dk_state.h must stay deko3d-free (T14–T16 review rule) → the declaration goes in a
  new dk-internal header, exactly the T22a precedent.

## Edits
1. NEW `game/graphics/deko3d_renderer/dk_state_internal.h`: `#pragma once`,
   `#if defined(__SWITCH__)`, `#include <deko3d.h>`, `#include "dk_state.h"` (for
   `Blend`), `namespace dkpipe { void apply_blend_const(DkCmdBuf cb, Blend b); }`.
2. `game/graphics/deko3d_renderer/dk_state.cpp`: implement — `case Blend::Fix64Sub:
   dkCmdBufSetBlendConst(cb, 0.5f, 0.5f, 0.5f, 0.5f); break;` all other Blend values:
   no-op. Keep the exhaustive switch (no default) so a future Blend value fails to
   compile here. Update the NOTE comment at :76 to point at this function.

## Done-check (background wrappers only)
`scripts/ai/check-bg.sh jak1` + `scripts/ai/check-tail.sh` → DONE-PASS.
Inert grep: `apply_blend_const` appears only in the two new/edited dk_state files.
QWEN_REPORT `## T26` (files + last 5 log lines + md5). README row → 🟡.
No commit, no deploy.
