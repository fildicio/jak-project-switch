# T33b — tfrag3 shader getters (internal header) (P3.2, AI-assisted)

Track D P3.2 · Depends on: T33a ✅ · Inert until T38 · One session (~25 min)

## Goal

Half 2 of T33. Expose the T33a-loaded shaders to sibling TUs via
`dk_device_internal.h` (the T22a pattern) so T38 can bind them without touching
dk_device.cpp again.

## Session rules (hard — read twice)

- Time box ~25 min. Read each listed range ONCE; re-reading = looping → STOP.
- FORBIDDEN reads: `deko3d.h`/`deko3d.hpp`, `docs/deko3d-ref/**`, session
  notes, the plan.
- Build command is exactly `scripts/ai/check-bg.sh jak1` — jak1, never jak2.
- Report goes in QWEN_REPORT.md under `## T33b`.

## Read (once, exactly this)

1. `game/graphics/pipelines/dk_device_internal.h` — whole file (it is short).
2. `grep -n 'internals' game/graphics/pipelines/dk_device.cpp` → read the
   internals section (one-liner accessors live there).

## Steps

1. In `dk_device_internal.h` (namespace `dkdev`), beside the existing
   accessors:
   ```cpp
   // T33b: tfrag3 shader pair loaded by T33a (valid only when
   // tfrag3_shaders_loaded() returns true — copies, not references).
   DkShader tfrag3_vsh();
   DkShader tfrag3_fsh();
   bool tfrag3_shaders_loaded();
   ```
2. One-liner implementations in dk_device.cpp's internals section: return
   `s_tfrag3Vsh` / `s_tfrag3Fsh` / `s_have_tfrag3`.
3. `shutdown()`: re-zero `s_have_tfrag3` (nothing else — `DkShader` has no
   destroy; the memblock teardown already exists).

## Rules

- Only `dk_device_internal.h` + `dk_device.cpp`. Nothing calls the getters yet
  (T38 does) — they exist to be gc-strippable.

## Done-check

```sh
scripts/ai/check-bg.sh jak1 && scripts/ai/check-tail.sh
```

DONE-PASS + NRO md5 `ce41507cb3319875fcb0a1ffe051b82d` UNCHANGED (uncalled
getters strip). Changed → explain before marking done.

## Doubts

- If the internals section convention is pass-by-pointer instead of value for
   DkShader, follow whatever the existing accessors do and note it.
