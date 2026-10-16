# T14 — `PipelineTable.h`: plain structs for the 224 pipelines

**Goal:** P3 prep. A deko3d-free description of every render state listed in
`docs/deko3d/pipeline_table.json`, so a generator (T15) can emit it as C++ data.

**Read first:** `docs/deko3d/pipeline_table.md` and one entry in `pipeline_table.json`:
`pipelines[i]` has `id`, `key`, `depth_test` (`OFF` or a compare name), `depth_write`,
`blend` (`OFF` or a mode name), `alpha_test`, `deko3d_state` (`depthTestEnable`,
`depthCompareOp`, `depthWriteEnable`, `blendEnable`, `blend`). `samplers[i]` has
`index`, `clamp_s`, `clamp_t`, `filt`, `mipmap`, `deko3d` (wrapU/V, min/magFilter).
List every distinct value of `depthCompareOp` and of `blend` (when not null) with a
short python one-liner and put them in your report.

**Create (1 file):** `game/graphics/deko3d_renderer/PipelineTable.h` — host-compilable,
no deko3d include:
```cpp
#pragma once
// deko3d PipelineTable (AI-assisted). Generated data: PipelineTable.inc (T15).
#include <cstdint>
namespace dkpipe {
enum class Compare : uint8_t { /* one value per distinct depthCompareOp, + Always */ };
enum class Blend : uint8_t { Off, /* one per distinct blend mode */ };
enum class Wrap : uint8_t { Repeat, ClampToEdge };
enum class Filter : uint8_t { Nearest, Linear };
struct PipelineDesc { const char* id; bool depth_test; Compare depth_op; bool depth_write; Blend blend; };
struct SamplerDesc { uint8_t index; Wrap wrap_u, wrap_v; Filter min_f, mag_f; bool mipmap; };
}
```
Add a one-line comment on each `Blend` value saying the GL equation it stands for
(take it from `pipeline_table.md`).

**Done-check:**
```sh
clang++ -std=c++17 -fsyntax-only -Wall -x c++ game/graphics/deko3d_renderer/PipelineTable.h && echo syntax-ok
```
