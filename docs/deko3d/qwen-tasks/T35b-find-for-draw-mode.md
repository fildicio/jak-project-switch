# T35b v2 — dkpipe find_for_draw_mode + sampler_idx (P3.2, AI-assisted)

Track D P3.2 · Depends on: T35a ✅ · Inert until T38 · One session (~30 min)
v2 (2026-10-19, reviewer): v1 was an open design prompt and sent you hunting a
`DrawMode` struct that does NOT exist in C++ (it is a GOAL type) — two sessions
died on it. This version SPELLS OUT the entire function. Transcribe it.

## Goal

Pure lookup over the T15 table: map already-derived draw axes to a
`PipelineDesc` entry + a `kSamplers` index. NO bit derivation here — the caller
(T38) derives the axes from the GOAL draw-mode word per
`docs/deko3d/pipeline_table.md` L27 table. No GPU calls, no allocation.

## Session rules (hard — read twice)

- Time box ~30 min. Over budget → STOP at the last clean compile; report.
- The code below IS the spec. Do not rename, generalize or "improve" it.
  If something looks wrong: implement as written, list it under Doubts.
- FORBIDDEN reads: `deko3d.h`/`deko3d.hpp`, `docs/deko3d-ref/**`, session
  notes, the plan, and ANY search for `DrawMode` (GOAL type, not C++).
- Read each listed range ONCE. Re-reading = looping → STOP, report.
- Build command is exactly `scripts/ai/check-bg.sh jak1` — jak1, never jak2.
- Report goes in QWEN_REPORT.md under `## T35b`.

## Read (once, exactly these)

1. `game/graphics/deko3d_renderer/PipelineTable.h` — whole file (66 lines):
   `PipelineDesc` (id, depth_test, depth_op, depth_write, blend) and
   `SamplerDesc` (index, wrap_u, wrap_v, min_f, mag_f, mipmap).
2. `game/graphics/deko3d_renderer/PipelineTable.inc` — first 3 entries of each
   array (shape only; 224 pipeline entries, 16 sampler entries).
3. `game/graphics/deko3d_renderer/dk_state.h` — current declarations (you add
   one struct + one function beside them).

## Steps (transcribe exactly)

1. In `dk_state.h`, inside `namespace dkpipe`, after `record_graphics_state`:

```cpp
// T35b: the draw axes the caller derives from the GOAL draw-mode word
// (derivation spec: docs/deko3d/pipeline_table.md L27 table — T38's job).
// Pure data, dkpipe types only, host-compilable.
struct DrawModeAxes {
  bool depth_test;   // GS Z-test enable
  Compare depth_op;  // when depth_test is false, pass Compare::Always (table rule)
  bool depth_write;
  Blend blend;       // 7-way table blend
  bool clamp_s;      // sampler wrap axes (false = Repeat)
  bool clamp_t;
  bool filt;         // false = Nearest, true = Linear (min and mag, per table)
  bool mipmap;
};

// First kPipelines entry whose 4 state fields equal the axes, else nullptr.
// Entries differing only in alpha-test share one PipelineDesc (alpha-test is
// shader-side), so first-match is exact. sampler_idx (optional out) = the
// kSamplers index whose 5 fields equal the sampler axes; untouched if none.
const PipelineDesc* find_for_draw_mode(const DrawModeAxes& ax,
                                       uint8_t* sampler_idx = nullptr);
```

2. In `dk_state.cpp`: add `#include "game/graphics/deko3d_renderer/PipelineTable.inc"`
   beside the existing `dk_state_internal.h` include (the arrays are `static`
   per TU — a second copy beside dk_device.cpp's is intended), then after
   `record_graphics_state` implement:

```cpp
const PipelineDesc* find_for_draw_mode(const DrawModeAxes& ax,
                                       uint8_t* sampler_idx) {
  Wrap wu = ax.clamp_s ? Wrap::ClampToEdge : Wrap::Repeat;
  Wrap wv = ax.clamp_t ? Wrap::ClampToEdge : Wrap::Repeat;
  Filter ft = ax.filt ? Filter::Linear : Filter::Nearest;
  for (const PipelineDesc& p : kPipelines) {
    if (p.depth_test == ax.depth_test && p.depth_op == ax.depth_op &&
        p.depth_write == ax.depth_write && p.blend == ax.blend) {
      if (sampler_idx) {
        for (uint32_t i = 0; i < sizeof(kSamplers) / sizeof(kSamplers[0]); ++i) {
          const SamplerDesc& s = kSamplers[i];
          if (s.wrap_u == wu && s.wrap_v == wv && s.min_f == ft &&
              s.mag_f == ft && s.mipmap == ax.mipmap) {
            *sampler_idx = static_cast<uint8_t>(i);
            break;
          }
        }
      }
      return &p;
    }
  }
  return nullptr;
}
```

3. Report: paste your two hunks, then state which `kPipelines` id the scan
   returns for ax = {false, Compare::Always, false, Blend::Off, false, false,
   false, false} (read the .inc entries you need — expected `P001`).

## Rules

- Only `dk_state.h` / `dk_state.cpp`. Pure logic, no GPU calls, no allocation.
- No hardcoded pipeline/sampler indices or id strings anywhere.

## Done-check

```sh
scripts/ai/check-bg.sh jak1 && scripts/ai/check-tail.sh <log>
```

DONE-PASS required. The NRO md5 WILL MOVE off `67018cd5a4411b3f3cc77f198773b261`
(expected: dk_state.cpp has been live-linked since T34a called `to_deko3d`).
That is NOT a stop: A/B-attribute (stash ONLY your two T35b files → rebuild →
compare → restore → rebuild) and report both md5s. STOP only if the A/B does
not attribute cleanly.

## Doubts

- Anything that looks wrong in the spelled-out code: implement as written and
  list it here. Do not redesign.
