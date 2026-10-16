# T16 — Convert a `PipelineDesc` into deko3d state structs

**Goal:** the bridge from the generated table (T14/T15) to real deko3d state, used by
every ported renderer later. Switch-only.

**Create (2 files):**
- `game/graphics/deko3d_renderer/dk_state.h` — includes `<deko3d.h>` and `PipelineTable.h`:
  ```cpp
  #pragma once
  #if defined(__SWITCH__)
  #include <deko3d.h>
  #include "game/graphics/deko3d_renderer/PipelineTable.h"
  namespace dkpipe {
  struct DkStates { DkDepthStencilState ds; DkColorState cs; DkBlendState bs; };
  //! Fill deko3d state for one table entry. Pure function, no GPU calls.
  void to_deko3d(const PipelineDesc& p, DkStates* out);
  //! Fill a DkSampler for one table sampler.
  void to_deko3d(const SamplerDesc& s, DkSampler* out);
  }
  #endif
  ```
- `game/graphics/deko3d_renderer/dk_state.cpp` — implementation, whole file in
  `#if defined(__SWITCH__)`. Start each struct from its `dk*Defaults()` function.
  Map `Compare` → `DkCompareOp_*`, each `Blend` → `dkBlendState*`/`DkBlendFactor_*`
  fields matching the GL equation commented in `PipelineTable.h`, `Wrap`/`Filter` →
  `DkWrapMode_*`/`DkFilter_*`, mipmap → `mipFilter` (check the field name).
  Use a `switch` with every enum value and no `default:` so the compiler warns on gaps.
  **Every deko3d field and enum name must be checked in `deko3d.h`.** In your report,
  list each `Blend` value with the GL equation and the deko3d factors you chose.

**Also edit (1 line):** `game/CMakeLists.txt` — add `graphics/deko3d_renderer/dk_state.cpp`
right after `graphics/pipelines/dk_device.cpp` in the source list.

**Done-check:** `scripts/ai/check-switch.sh jak1` → `PASS`.
