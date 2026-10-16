# T22 — TexturePoolDeko3D GPU half: DkImage create + BCn direct upload (P2.3)

Goal: the GPU half of the texture store (T19 wrote the bookkeeping half). Creates one
`DkImage` per texture in a dedicated image memblock, writes its image descriptor into a
grown descriptor region, and uploads pixels — RGBA8 through staging, BCn (fr3 v44, FIX 74)
DIRECT from the card bytes with zero CPU decode. **Inert**: nothing calls it yet (T23 does).

## Rules (read first)
- **Verify EVERY deko3d name in `docs/deko3d-ref/include/deko3d.h` before using it** —
  grep it, do not trust memory. Known-good anchors: `dkImageLayoutMakerDefaults` (L568),
  `dkImageLayoutInitialize/GetSize/GetAlignment` (L1339–41), `dkImageInitialize` (L1343),
  `dkImageDescriptorInitialize` (L1346), `dkCmdBufCopyBufferToImage` (L1318),
  `dkCmdBufBarrier` (L1259), `DkCopyBuf` struct (L1199, plain struct — addr/length/
  rowPitch, no maker function). Also grep and use: `dkImageViewDefaults`,
  `DkImageFormat_RGB_BC1`, `DkImageFormat_RGBA_BC3`, `DkImageFormat_RGBA_BC7_Unorm`,
  `DkImageLayoutMaker` fields, the `DkInvalidateFlags` image flag for the post-copy
  barrier, `DK_IMAGE_DESCRIPTOR_ALIGNMENT`.
- Only `dk_*.cpp` files include `<deko3d.h>`; headers stay plain C++.
- Never edit `game/graphics/opengl_renderer/` or any desktop path.
- Fenced uploads only (T18 pattern); NEVER `dkQueueWaitIdle`.
- Log format: `[dk]` prefix, one line.
- STOP rule: if any command fails, append ONLY its last 5 lines to your report, do NOT
  attempt fixes, do NOT paste long logs — report STOP instead.

## Edits

### 1. `game/graphics/pipelines/dk_device.cpp` — grow the descriptor region
- `constexpr uint32_t kMaxTexImages = 2048;`
- `kDescBytes`: `0x100` → `0x100 + kMaxTexImages * DK_IMAGE_DESCRIPTOR_ALIGNMENT`
  (= 0x40100). Splash keeps the first 0x100 (2 descriptors, unchanged offsets);
  image-descriptor slot i lives at `kOffDesc + 0x100 + i * DK_IMAGE_DESCRIPTOR_ALIGNMENT`.
  `kOffUbo`/`kDataPool` shift automatically (derived constants) — the existing
  static_asserts must still hold (verify: kOffUbo = 0x624100 stays 0x100-aligned).
  Update the layout comment.

### 2. NEW `game/graphics/pipelines/dk_device_internal.h` (dk-TU-internal, deko3d-typed)
`#pragma once` + `#if defined(__SWITCH__)` + `#include <deko3d.h>`; declares (in
`namespace dkdev`) the internals shared with other dk_*.cpp files:
`DkDevice device_handle(); DkQueue queue_handle(); DkCmdBuf cmd_handle();`
`uint8_t* staging_cpu(); DkGpuAddr staging_gpu();`
`void submit_upload_cmds(DkCmdList list);`  // submit + SignalFence(s_uploadFence, true) + dkFenceWait — reuses the T18 fenced-upload pattern; never invent a second fence

### 3. `game/graphics/pipelines/dk_device.cpp` — implement those six
(trivial pass-throughs; `submit_upload_cmds` is exactly the T18 submit/fence/wait tail).

### 4. NEW `game/graphics/deko3d_renderer/dk_texture_gpu.h` (plain header)
`class TexturePoolDeko3D` (namespace `dktexgpu`): owns a `dktex::TextureStore` (T19) +
fixed arrays of `DkImage`/`DkImageLayout` per slot + the image memblock handle — all
behind a plain API; the deko3d types live only in the .cpp (store them as opaque
aligned storage in the header, or keep the arrays in the .cpp as file-static state —
prefer the latter: a singleton `dktexgpu::init()/teardown()` free-function API, like dkdev).
Public API (plain types only):
- `bool init();` — creates the image memblock (48 MB, `GpuCached | Image`, flags like
  the T05 code-memblock maker), capacity `kMaxTexImages`. Failure → log
  `[dk] tex pool UNUSABLE (memblock failed)` and stay disabled (never fatal).
- `uint32_t upload_rgba8(uint32_t w, uint32_t h, const void* src);`
- `uint32_t upload_bcn(dktex::Fmt fmt, uint32_t w, uint32_t h, const void* src, uint32_t size);`
  Both: alloc id from the store (0 → return 0, caller skips upload — FIX 33 contract);
  compute BCn payload size yourself only as a sanity check (`size` from the caller wins).
- `bool add_level_ref(uint32_t id); void release(uint32_t id);`  (straight store pass-through)
- `bool active();` Telemetry: `uint32_t live_count(); uint32_t high_water(); int failed_allocs();
  uint64_t image_bytes_used();` (for T24's soak lines).
- `void teardown();` — destroy the memblock.

### 5. NEW `game/graphics/deko3d_renderer/dk_texture_gpu.cpp`
`#if defined(__SWITCH__)`, includes `<deko3d.h>`, `dk_device_internal.h`, `dk_texture_gpu.h`.
Per upload (RGBA8 and BCn alike):
1. Store alloc → slot.
2. `DkImageLayoutMaker` (`dkImageLayoutMakerDefaults` first; set width/height/mipLevels=1/
   format — RGBA8: `DkImageFormat_RGBA8_Unorm`; BCn per Fmt; `flags 0`, `samples 1` —
   grep the struct for exact field names), `dkImageLayoutInitialize`, then bump-allocate
   in the image memblock at `dkImageLayoutGetAlignment` alignment; if
   `dkImageLayoutGetSize` doesn't fit what's left → log
   `[dk] tex pool full (image bytes %llu MB)` , release the id, return 0.
3. `dkImageInitialize` at that offset; `dkImageViewDefaults` + view of the image;
   `dkImageDescriptorInitialize(&desc, &view, false, false)`; memcpy the descriptor to
   the slot's offset in the data memblock (see §1).
4. Stage the payload: assert `size <= kStagingMax` (2 MB — level textures fit; if bigger,
   fail with a log line, release id, return 0), memcpy to `staging_cpu()`, then on
   `cmd_handle()`: `dkCmdBufClear` → `dkCmdBufAddMemory(s_dataMem, kOffUpload, kCmdUpload)`
   (expose kOffUpload/kCmdUpload via the internal header too) → `dkCmdBufCopyBufferToImage`
   (DkCopyBuf{staging_gpu, size, rowPitch} — grep the struct fields for RGBA8 vs BCn
   rowPitch semantics; dstView = the view; dstRect covers the whole image; flags 0) →
   `dkCmdBufBarrier` invalidating image caches (grep DkInvalidateFlags for the flag name)
   → `submit_upload_cmds(dkCmdBufFinishList(cmd_handle()))`.
5. Log `[dk] tex id=%u %ux%u %s %u KB` (fmt name) once per upload.
Add the .cpp next to `dk_renderer.cpp` in the same Switch CMake source list (same as the
T14–16 `dk_state.cpp` addition — find that list entry and copy it).

## Done-check (background wrappers ONLY)
```
scripts/ai/check-bg.sh jak1
scripts/ai/check-tail.sh
```
Then: `grep -rn 'dktexgpu::' game/ --include='*.cpp' | grep -v dk_texture_gpu` must be
empty (inert). QWEN_REPORT `## T22` section: files changed, every deko3d name you used
with the header line you verified it at, last 5 log lines + md5. Flip README row to 🟡.
No commit, no deploy. If the build fails twice, STOP per the rule above.
