# T34a — sampler descriptor set: layout + init loop (P3.2, AI-assisted)

Track D P3.2 · Depends on: T16 ✅ · Inert until T38 · One session (~40 min)
Split from T34 (halving pass, 2026-10-18).

## Goal

Half 1 of T34. Append the sampler-descriptor region to the data pool (AFTER the
UBO ring — never renumber existing offsets) and write the game's sampler table
into it at init. No accessors yet (T34b).

## Session rules (hard — read twice)

- Time box ~40 min. Over budget → STOP at the last clean compile; report what remains.
- Read each listed range ONCE. Re-reading = looping → STOP, report.
- FORBIDDEN reads: `deko3d.h`/`deko3d.hpp`, `docs/deko3d-ref/**`, session
  notes, the plan. Everything is pasted below.
- Build command is exactly `scripts/ai/check-bg.sh jak1` — jak1, never jak2.
- Report goes in QWEN_REPORT.md under `## T34a`.

## Read (once, exactly these)

1. `game/graphics/pipelines/dk_device.cpp` L44-67 — the data-pool layout
   constants + comment block (current order: cmd | upload cmd | staging |
   descriptors | UBO).
2. `game/graphics/deko3d_renderer/PipelineTable.h` L55-70 — `SamplerDesc`.
3. `grep -n 'kSamplers' game/graphics/deko3d_renderer/PipelineTable.inc` —
   note the entry count for your loop bound (T34b reports the exact max index).
4. `grep -n 'DescriptorInitialize' game/graphics/pipelines/dk_device.cpp` →
   read the splash descriptor-write block (your template for the write pattern).

## API (pasted verbatim from deko3d.h — do not open it)

```c
#define DK_SAMPLER_DESCRIPTOR_ALIGNMENT 0x20    // L154 (exists — use it)
void dkSamplerDescriptorInitialize(DkSamplerDescriptor* obj,
                                   DkSampler const* sampler);      // L1348
```

- Fill source: `dkpipe::to_deko3d(const SamplerDesc&, DkSampler*)` — declared
  `game/graphics/deko3d_renderer/dk_state.h` L25 (T16, already reviewed). Call
  it; do not touch dk_state.cpp.

## Steps

1. Layout constants: `kOffSamplers = <old kDataPool end>` (i.e. UBO end),
   `kSamplersBytes = count * DK_SAMPLER_DESCRIPTOR_ALIGNMENT`; grow the pool
   size constant by `kSamplersBytes`. Update the layout comment line to
   `[cmd | upload cmd | staging | descriptors | UBO | samplers]`. Move NOTHING.
2. Init, right after the splash descriptors: loop `i < count` over `kSamplers[]`:
   `dkpipe::to_deko3d(kSamplers[i], &s)` → `dkSamplerDescriptorInitialize`
   into `cpu_base + kOffSamplers + i * DK_SAMPLER_DESCRIPTOR_ALIGNMENT`.
   (`dkSamplerDescriptorInitialize` cannot fail; the only failure mode is a
   null sampler from bad input — if `to_deko3d` returns/fails oddly, one
   `[dk] sampler table init failed at i` line and continue.)
3. Pool-size accounting: the memblock SIZE rounds to 0x1000 automatically —
   verify the pool constant still 0x1000-rounds (it must; say so in report).

## Rules

- Only `dk_device.cpp` (+ the two bounded reads). 512 B growth. Never move an
  existing offset — if a constant must move to accommodate, STOP and report.

## Done-check

```sh
scripts/ai/check-bg.sh jak1 && scripts/ai/check-tail.sh
```

DONE-PASS + NRO md5: your init-loop edit is inside LIVE boot code, so the md5 WILL
move off `e819b40d2be605b19099288c9f215ecf` — that is EXPECTED (T33a precedent).
Do NOT stop for the move: A/B-attribute (revert/stash YOUR files → rebuild → compare),
paste both md5s in the report, and confirm your diff is additive-only (no call-graph
change, no offset moved). STOP only if the A/B does not attribute cleanly.

## Doubts

- If the real `kSamplers[]` count is not 16, that is FINE — loop over what is
   there and report the number (T34b records it).
