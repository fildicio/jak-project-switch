# T34 — sampler descriptor set: 16-combo table → data pool (P3.2, AI-assisted)

Track D P3.2 · Depends on: T16 ✅ (dkpipe::to_deko3d) · Inert until T38 · One session

## Goal

Create the game's 16-combo sampler descriptor set (FIX 43's table) in the data
pool at init: one `DkSamplerDescriptor` per `dkpipe::SamplerDesc` entry of
`kSamplers`, at a NEW region appended AFTER the UBO ring (never renumber existing
offsets — the T21/T22b lesson). Expose its GPU address for
`dkCmdBufBindSamplerDescriptorSet` at draw time.

## Context

- `game/graphics/deko3d_renderer/PipelineTable.h` — `SamplerDesc` (L59-64);
   `PipelineTable.inc` — `kSamplers[]` (grep it; note the max index — the set
   size is `maxSamplerIndex+1`, usually 16).
- `game/graphics/deko3d_renderer/dk_state.h` — `dkpipe::to_deko3d(const
  SamplerDesc&, DkSampler*)` (L25): your fill function, already reviewed in T16.
- `game/graphics/pipelines/dk_device.cpp` L53-67 — the data-pool layout
  `[cmd | upload cmd | staging | descriptors | UBO]` with the offsets; you append
  `[samplers]` at the end: `kOffSamplers = kOffUbo + kUboBytes` (the old
  `kDataPool`), then grow `kDataPool += kSamplers * DK_SAMPLER_DESCRIPTOR_ALIGNMENT`.
- Splash precedent: descriptor writes at init in dk_device.cpp (T06 block — grep
  `dkImageDescriptorInitialize` / `dkSamplerDescriptorInitialize` usage).

## API cheat-sheet (verified)

- `dkSamplerDescriptorInitialize(&desc, &sampler)` — deko3d.h:1348.
- Alignment: grep `DK_SAMPLER_DESCRIPTOR_ALIGNMENT` in deko3d.h — if that exact
  name does not exist, use 0x20 (same as image descs) WITH a
  `static_assert(kOffSamplers % 0x20 == 0)` and say so in the report.
- Bind at draw time (T38, not here): `dkCmdBufBindSamplerDescriptorSet(cb,
  gpuAddr, count)` — deko3d.h:1266.
- Handle composition (T38, not here): `dkMakeTextureHandle(imageId, samplerId)`
  (deko3d.h:715) — samplerId indexes THIS set, imageId indexes the image set
  bound from `kOffDesc` (see T22b layout note in dk_device.cpp L56-61: texture i
  sits at `kOffDesc + 0x100 + i*0x20`, i.e. image id `8 + i` when the set base
  is `kOffDesc` — the splash image occupies ids 0-7).

## Steps

1. dk_device.cpp: add the `kOffSamplers`/`kSamplersBytes` constants per the
   layout rule above; update the layout comment block (L44-54) — the order line
   becomes `[cmd slices | upload cmd | staging | descriptors | UBO | samplers]`.
2. At init, after the T06 splash descriptors: loop `kSamplers[]`, call
   `dkpipe::to_deko3d(sdesc, &sampler)` + `dkSamplerDescriptorInitialize` into
   `(uint8_t*)dkMemBlockGetCpuAddr(s_dataMem) + kOffSamplers + i * alignment`.
   Any failure of the pattern (only possible via bad input — init calls can't
   fail here): one `[dk] sampler table init failed at i` line, keep going, return
   count via the accessor.
3. Expose in `dk_device_internal.h`: `uint32_t sampler_set_off();
   uint32_t sampler_set_count();` one-liners (T22a pattern).
4. `s_pool_bytes` accounting already covers the whole memblock — no change.

## Rules

- Only `dk_device.cpp` + `dk_device_internal.h`. (dk_state.cpp is NOT touched —
  to_deko3d is already correct.)
- Data-pool growth is 16*0x20 = 512 B — confirm kDataPool stays 0x1000-aligned
  (it is a memblock SIZE, so rounding is fine) — do not move ANY existing offset.

## Done-check

```sh
scripts/ai/check-bg.sh jak1 && scripts/ai/check-tail.sh
```

DONE-PASS + NRO md5 unchanged (`74635895…`): init writes 512 B of descriptors
into an existing memblock and the new accessors are uncalled → inert. Changed →
explain before marking done.

## Doubts

- If `kSamplers[]` max index is not 15, report the real count — the "16-combo"
  label came from FIX 43 and must match reality.
