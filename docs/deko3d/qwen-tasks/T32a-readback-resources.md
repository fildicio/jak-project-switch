# T32a — dkrt readback queue + staging (P3.1 slice 6a, AI-assisted)

Track D P3.1 · Depends on: T29b ✅ · Inert · One session (~40 min)

## Goal

Half 1 of T32. The RESOURCES only, no readback call yet: a lazy dedicated
`DkQueue` + its `DkCmdBuf` + a CPU-visible staging memblock, owned by dkrt,
destroyed in teardown. (`readback_rgba8()` itself is T32b.)

## Session rules (hard — read twice)

- Time box ~40 min. Over budget → STOP at the last clean compile; report what remains.
- Read each listed range ONCE. Catch yourself re-reading → you are looping → STOP, report.
- FORBIDDEN reads: `deko3d.h`/`deko3d.hpp`, `docs/deko3d-ref/**`, session
  notes, the plan. Everything is pasted below.
- Build command is exactly `scripts/ai/check-bg.sh jak1` — jak1, never jak2.
- Report goes in QWEN_REPORT.md under `## T32a`.

## Read (once, exactly this)

1. `grep -n 'DkQueueMaker\|dkQueueCreate\|dkCmdBufAddMemory' \
   game/graphics/pipelines/dk_device.cpp` → read the ONE function that creates
   the main queue/cmdbuf (usually ~15 lines). Copy its style, not its values.

## API (pasted verbatim from deko3d.h — do not open it)

```c
typedef struct DkQueueMaker {                       // L271-279
    DkDevice device; uint32_t flags; uint32_t commandMemorySize;
    uint32_t flushThreshold; uint32_t perWarpScratchMemorySize;
    uint32_t maxConcurrentComputeJobs;
} DkQueueMaker;
DkQueue dkQueueCreate(DkQueueMaker const* maker);   // L1324  — check result
typedef struct DkCmdBufMaker {                      // L245-250
    DkDevice device; void* userData; DkCmdBufAddMemFunc cbAddMem;
} DkCmdBufMaker;
DkCmdBuf dkCmdBufCreate(DkCmdBufMaker const* maker);// L1246  — check result
// dkQueueMakerDefaults(maker, device) / dkCmdBufMakerDefaults(maker, device)
// exist as static inline in the same header — call them, then override.
// dkCmdBufAddMemory(cb, memblock, offset, size) — see how dk_device.cpp uses it.
```

- Memblock for staging: flags `CpuUncached | GpuCached`, size `w*h*4` rounded
  up to 0x1000. Cmd memory: 64 KB (0x10000) region of a second same-flagged
  memblock (or one memblock, two regions — your call, comment it).

## Steps

1. `dk_render_target.cpp`, new namespace-level section `// ---- readback (T32) ----`:
   file-static handles `s_rbQueue/s_rbCmdBuf/s_rbCmdMem/s_rbStageMem/...` (all
   zero-init), plus `ensure_readback_resources(uint32_t w, uint32_t h)`:
   - already-created + big enough → return true immediately.
   - create/destroy-and-recreate per the sizes; EVERY create checked
     (`dkQueueCreate` result is a `DkResult`, `dkMemBlockCreate` too); on any
     failure: one `[dk] rt readback resources failed (<what>)` line, full
     unwind, return false.
2. `dkrt::teardown()`: destroy queue → cmdbuf → memblocks in that order, all
   null-safe (never created must be fine). One `[dk] rt readback teardown` line
   only when something was actually destroyed.
3. NOTHING calls `ensure_readback_resources` yet (T32b does). No header changes
   (all file-static) — if you find you need a header decl, your design drifted:
   STOP and report.

## Rules

- Only `dk_render_target.cpp`. Checked creates, one-line `[dk]` failures, never
  crash. NO `std::thread` (the dedicated queue IS the answer). Stays inert.

## Done-check

```sh
scripts/ai/check-bg.sh jak1 && scripts/ai/check-tail.sh
```

DONE-PASS + NRO md5 `ce41507cb3319875fcb0a1ffe051b82d` UNCHANGED (uncalled →
gc-strips). Changed → STOP.

## Doubts

- If dk_device.cpp's queue creation sets fields the paste above lacks (e.g. a
  flush threshold that matters), copy that exact field/value and note it.
