# T18 — `dkdev::copy_to_gpu`: staging copy + fence upload primitive

**Goal:** P2.2 core. The runtime upload primitive every later texture/buffer upload
uses: copy CPU bytes to any GPU address inside the dkdev memblocks, through the
staging area, on the existing queue, synchronized with a **fence** (NOT
`dkQueueWaitIdle` — the 30 fps frame loop must never stall). Nothing calls it yet
(loader integration is a later task): this lands the primitive, verified by build.

**Read first:** `game/graphics/pipelines/dk_device.cpp` — `upload_splash` (the
boot-time pattern you adapt), the constants block at the top (`kCmdSlice`,
`kOffStaging`, `kStagingMax`, the layout comment `[cmd slices | staging | descriptors
| UBO]`), `present_frame` (how `s_sliceFence` is waited on), and how `s_sliceFence`
is initialized/fenced today. Copy that pattern exactly. Also
`game/graphics/pipelines/dk_device.h` (the plain, deko3d-free interface comment).

**Real API names (verified in `docs/deko3d-ref/include/deko3d.h`):**
```c
void dkCmdBufCopyBuffer(DkCmdBuf obj, DkGpuAddr srcAddr, DkGpuAddr dstAddr, uint32_t size);  // line 1314
void dkQueueSubmitCommands(DkQueue obj, DkCmdList cmds);                                    // line 1329
void dkQueueSignalFence(DkQueue obj, DkFence* fence, bool flush);                           // line 1328
// dkCmdBufClear / dkCmdBufAddMemory / dkCmdBufFinishList / dkFenceWait: already used in dk_device.cpp.
```
There is **no** `dkQueueCopyBuffer` in deko3d 0.5.0 — don't look for one.

**Edit (2 files):**

1. `game/graphics/pipelines/dk_device.h` — add inside `namespace dkdev`:
```cpp
struct CopyResult {
  bool ok = false;
  double ms = 0.0;   // total wall time including the fence wait
};
//! Copy `size` bytes from CPU memory to `gpu_dst` (must be inside a dkdev memblock),
//! through the staging area, in chunks of kStagingMax, fenced per chunk. Blocking.
//! Render thread only. Returns {false,0} when the device is down or args are bad.
CopyResult copy_to_gpu(uint64_t gpu_dst, const void* src, uint32_t size);
```

2. `game/graphics/pipelines/dk_device.cpp`:
   - New constant `kCmdUpload = 0x4000` (16 KB) — a command-memory region dedicated
     to uploads, inserted in the layout **before** staging: `[cmd slices | upload cmd
     | staging | descriptors | UBO]`. Update the layout comment and derive
     `kOffStaging` etc. from it (all offsets are derived constants; the existing
     static_asserts must still hold).
   - `static DkFence s_uploadFence` initialized the same way `s_sliceFence` is.
   - Implement `copy_to_gpu`:
     - Guards: `!is_up()`, `src == nullptr`, `size == 0` → `{false, 0}`.
     - Loop over chunks of `kStagingMax`: `memcpy` into staging at `kOffStaging`;
       `dkCmdBufClear(s_cmd); dkCmdBufAddMemory(s_cmd, s_dataMem, kCmdSlice2_offset,
       kCmdUpload)` (the upload region's offset — NOT slice 0, frames in flight own
       the per-frame slices); record `dkCmdBufCopyBuffer(s_cmd, staging_gpu,
       gpu_dst + offset, chunk)`; `dkQueueSubmitCommands(s_queue, dkCmdBufFinishList(s_cmd))`;
       `dkQueueSignalFence(s_queue, &s_uploadFence, /*flush*/ true)`;
       `dkFenceWait(&s_uploadFence, -1)` — waiting before the next chunk makes the
       upload command memory safe to reuse (same rule as the frame slices).
     - Time the whole call with the existing tick→ms helper used by `FrameTiming`.
     - One `[dk]` log line per call: `[dk] copy_to_gpu %u KB in %u chunks, %.3f ms`
       (only when it succeeds; on failure log why).
   - Keep everything inside the existing `#if defined(__SWITCH__)` structure of the file.

**Done-check** (Docker build — wrappers only, never foreground):
```sh
scripts/ai/check-bg.sh jak1
# then repeat until DONE-PASS or DONE-FAIL:
scripts/ai/check-tail.sh
```
Expected: `DONE-PASS`. Then `grep -n copy_to_gpu game/graphics/pipelines/dk_device.h
game/graphics/pipelines/dk_device.cpp` and paste the lines.

**If the done-check is not DONE-PASS: STOP.** Append to your QWEN_REPORT.md section
the last 5 lines of `check-tail.sh` output and what you had changed — do not attempt
further fixes and do not read the whole build log into your session (long logs kill
the session).

**When done:** flip your row in `docs/deko3d/qwen-tasks/README.md` to 🟡 and append a
`## T18` section to `docs/deko3d/qwen-tasks/QWEN_REPORT.md` (what you did + the
done-check last lines).
