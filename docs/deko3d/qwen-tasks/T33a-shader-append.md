# T33a — tfrag3 dksh pair: embed + append into s_codeMem (P3.2, AI-assisted)

Track D P3.2 · Depends on: T25 ✅ + T24 ✅ · Inert until T39 · One session (~40 min)
Split from T33 (halving pass, 2026-10-18).

## Goal

Half 1 of T33. Load the T25-built tfrag3 .dksh pair as `DkShader` objects into
the EXISTING 64 KB code memblock, 0x100-aligned after the splash shaders, with
an overflow check. No getters yet (T33b).

## Session rules (hard — read twice)

- Time box ~40 min. Over budget → STOP at the last clean compile; report what remains.
- Read each listed range ONCE. Re-reading = looping → STOP, report.
- FORBIDDEN reads: `deko3d.h`/`deko3d.hpp`, `docs/deko3d-ref/**`, session
  notes, the plan. Everything is pasted below.
- Build command is exactly `scripts/ai/check-bg.sh jak1` — jak1, never jak2.
- Report goes in QWEN_REPORT.md under `## T33a`.

## Read (once, exactly these)

1. `find build-switch-jak1 -name 'tfrag3_*_dksh.h' -o -name 'splash_vert.h'`
   — the generated headers must sit in the SAME directory (same EmbedBinary
   rule); if not, STOP and report the two paths.
2. `grep -n 'splash_vert\|s_codeMem\|kCodePool' game/graphics/pipelines/dk_device.cpp`
   → then read ONLY: the include block at the top (~L22), the state vars
   (~L108-112), and the splash shader init block (the first `dkShaderInitialize`
   hit). That init block is your template.

## API (pasted verbatim — do not open deko3d.h)

```c
#define DK_SHADER_CODE_ALIGNMENT 0x100      // L151
// dkShaderMakerDefaults(&maker, codeMem, codeOffset) sets:
//   maker->codeMem / maker->control = NULL / maker->codeOffset / programId = 0
// void dkShaderInitialize(DkShader* obj, DkShaderMaker const* maker);  // L1335
```

Copy the splash init block's exact maker/defaults/initialize/error-check shape.

## Steps

1. Includes: add the two generated headers next to the splash ones, SAME style.
2. State: `DkShader s_tfrag3Vsh, s_tfrag3Fsh; bool s_have_tfrag3 = false;`
   beside the splash shader state.
3. After splash init succeeds: compute `off = align_up(splash_code_end, 0x100)`
   (however dk_device.cpp tracks the end — if it does not track it, add a small
   static that does, mirroring the splash sizes). Append vert then frag (frag
   aligned up again). If `end > kCodePool`: ONE line
   `[dk] tfrag3 shader code overflow (N/65536)` and skip BOTH (device stays up,
   splash unaffected). Do NOT grow the pool silently.
4. On success set `s_have_tfrag3 = true`. Nothing reads it yet (T33b adds that).

## Rules

- Only `dk_device.cpp`. Checked calls, one-line `[dk]` failures, never crash.

## Done-check

```sh
scripts/ai/check-bg.sh jak1 && scripts/ai/check-tail.sh
```

Expected: DONE-PASS and NRO md5 **unchanged** `ce41507cb3319875fcb0a1ffe051b82d`
— the load code runs at boot but writes only memblock bytes and the flag is
never read → the linker may keep or strip it; if the md5 CHANGES, verify with
`git diff` that ONLY this init code was added (no call-graph change) and SAY SO
in the report. Anything beyond that → STOP.

## Doubts

- If the generated tfrag3 headers' symbol names differ from the splash ones'
   pattern, quote both declarations in the report before adapting.
