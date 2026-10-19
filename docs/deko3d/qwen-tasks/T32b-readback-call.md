# T32b — dkrt readback_rgba8() (P3.1 slice 6b, AI-assisted)

Track D P3.1 · Depends on: T32a ✅ · Inert · One session (~40 min)

## Goal

Half 2 of T32. The blocking readback call: record the copy, submit on the
dedicated queue, signal + wait a fence, memcpy staging→caller. Uses T32a's
resources — do not recreate anything.

## Session rules (hard — read twice)

- Time box ~40 min. Over budget → STOP at the last clean compile; report what remains.
- Read each listed range ONCE. Re-reading = looping → STOP, report.
- FORBIDDEN reads: `deko3d.h`/`deko3d.hpp`, `docs/deko3d-ref/**`, session
  notes, the plan. Everything is pasted below.
- Build command is exactly `scripts/ai/check-bg.sh jak1` — jak1, never jak2.
- Report goes in QWEN_REPORT.md under `## T32b`.

## Read (once, exactly this)

1. `grep -n 'submit_upload_cmds' game/graphics/pipelines/dk_device.cpp` → read
   that function body only (submit → signal fence → flush → wait pattern).

## API (pasted verbatim from deko3d.h — do not open it)

```c
void dkCmdBufCopyImageToBuffer(DkCmdBuf obj, DkImageView const* srcView,   // L1319
    DkImageRect const* srcRect, DkCopyBuf const* dst, uint32_t flags);
typedef struct DkCopyBuf {                    // L1199-1204 — EXACT names
    DkGpuAddr addr;      // staging GPU address
    uint32_t rowLength;  // row length in PIXELS (w for RGBA8)
    uint32_t imageHeight;
} DkCopyBuf;
void dkQueueSubmitCommands(DkQueue obj, DkCmdList cmds);   // L1329
void dkQueueSignalFence(DkQueue obj, DkFence* fence, bool flush);  // L1328
DkResult dkFenceWait(DkFence* obj, int64_t timeout_ns);    // L1240, -1 = forever
// cmd list finisher is dkCmdBufFinishList(DkCmdBuf) — see submit_upload_cmds.
```

**flags = 0** (the FlipX header comment says flip bits are "only for BlitImage
and CopyDataToImage" — CopyImageToBuffer is not listed; do not pass anything
else). NOTE: an older draft of this task claimed field names `address/rowPitch/
slicePitch` — WRONG; the names above are the real ones.

## Steps

1. `dk_render_target.h` (plain, no deko3d types):
   ```cpp
   namespace dkrt {
   // Blocking RGBA8 readback on the dedicated queue. view = opaque DkImageView*
   // (e.g. FullTarget::resolved_view()). out must hold w*h*4 bytes.
   // Caller must ensure the source is not being written (completed frame).
   bool readback_rgba8(const void* view, uint32_t w, uint32_t h, uint8_t* out);
   }
   ```
2. `.cpp`: guards (null view/out, w/h 0 → one `[dk] rt readback bad args` line,
   false). `ensure_readback_resources(w,h)` (T32a) — false → return false.
   Then record (fresh cmd buf slice): `DkImageRect{0,0,0,w,h,1}`, `DkCopyBuf`
   {staging addr, w, h}, copy flags 0 → finish list → submit → signal fence
   (flush=true) → `dkFenceWait(-1)` → memcpy staging→out → true.
3. Mirrors `submit_upload_cmds` exactly for the submit/signal/wait ordering.

## Rules

- Only `dk_render_target.h/.cpp`. One `[dk]` line per failure, never crash, no
  threads. Stays inert (no callers — T40/T4x wire it).

## Done-check

```sh
scripts/ai/check-bg.sh jak1 && scripts/ai/check-tail.sh
```

DONE-PASS + NRO md5 `ce41507cb3319875fcb0a1ffe051b82d` UNCHANGED. Changed → STOP.

## Doubts

- If `submit_upload_cmds` shows an extra step between submit and wait (e.g. an
  explicit flush call), copy it and note it — do not improvise.
