# T34b — sampler set accessors + count verify (P3.2, AI-assisted)

Track D P3.2 · Depends on: T34a ✅ · Inert until T38 · One session (~25 min)

## Goal

Half 2 of T34. Expose the sampler set to draw-time TUs via
`dk_device_internal.h`, and pin down the REAL table count (the "16-combo" label
came from FIX 43 and must match reality).

## Session rules (hard — read twice)

- Time box ~25 min. Read each listed range ONCE; re-reading = looping → STOP.
- FORBIDDEN reads: `deko3d.h`/`deko3d.hpp`, `docs/deko3d-ref/**`, session
  notes, the plan.
- Build command is exactly `scripts/ai/check-bg.sh jak1` — jak1, never jak2.
- Report goes in QWEN_REPORT.md under `## T34b`.

## Read (once, exactly these)

1. `game/graphics/pipelines/dk_device_internal.h` — whole file (short).
2. `game/graphics/deko3d_renderer/PipelineTable.inc` — the `kSamplers[]` block
   ONLY (`grep -n 'kSamplers' ` first; read the array, nothing else).

## API (needed at draw time — T38, NOT you; pasted for the comment)

```c
// void dkCmdBufBindSamplerDescriptorSet(DkCmdBuf, DkGpuAddr, uint32_t count);
// DK_CONSTEXPR DkResHandle dkMakeTextureHandle(uint32_t imageId,
//                                              uint32_t samplerId);  // L715
```

## Steps

1. In `dk_device_internal.h` (namespace `dkdev`):
   ```cpp
   // T34b: sampler descriptor set written at init by T34a.
   // GPU addr = data-pool base + sampler_set_off(); samplerId for
   // dkMakeTextureHandle indexes [0, sampler_set_count()).
   uint32_t sampler_set_off();
   uint32_t sampler_set_count();
   ```
2. One-liners in dk_device.cpp's internals section returning `kOffSamplers`
   and the count constant from T34a.
3. Add a `static_assert(kOffSamplers % DK_SAMPLER_DESCRIPTOR_ALIGNMENT == 0);`
   in dk_device.cpp if it does not already exist.
4. Report: the REAL max sampler index + count from `PipelineTable.inc`
   (is it 16 as FIX 43 said? quote the array tail).

## Rules

- Only `dk_device_internal.h` + `dk_device.cpp`. Uncalled accessors strip.

## Done-check

```sh
scripts/ai/check-bg.sh jak1 && scripts/ai/check-tail.sh
```

DONE-PASS + NRO md5: accessor-only additions usually keep `e819b40d2be605b19099288c9f215ecf`
(dkrt strip precedent), but ANY live-function edit (init/shutdown) moves it — expected.
If it moves: A/B-attribute (revert/stash YOUR files → rebuild → compare), paste both md5s.
STOP only if the A/B does not attribute cleanly.

## Doubts

- If the data-pool base GPU address is not exposed yet (sampler_set_off is an
   OFFSET, T38 adds it to the base accessor that already exists — check
   dk_device_internal.h for it), note which accessor T38 should use.
