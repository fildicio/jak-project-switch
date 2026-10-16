# T22b — dk_texture_gpu: store + DkImage + RGBA8 upload (P2.3)

Second slice of the old T22 (needs T22a ✅). Creates the GPU texture half for RGBA8;
BCn is T22c. **Inert**: nothing outside calls dktexgpu yet.

## API cheat-sheet — REVIEWER-VERIFIED against docs/deko3d-ref/include/deko3d.h
2026-10-16 (line numbers given). Do NOT re-grep these unless something fails to compile:
- `DkImageLayoutMaker` fields: `device, type, flags, format, msMode, dimensions[3], mipLevels, pitchStride`
- `dkImageLayoutMakerDefaults(DkImageLayoutMaker*, DkDevice)` L568 (defaults msMode=DkMsMode_1x, type 2D)
- `dkImageLayoutInitialize(DkImageLayout*, const DkImageLayoutMaker*)` L1339
- `uint64_t dkImageLayoutGetSize(const DkImageLayout*)` L1340; `uint32_t dkImageLayoutGetAlignment(...)` L1341
- `dkImageInitialize(DkImage*, const DkImageLayout*, DkMemBlock, uint32_t offset)` L1343
- `dkImageViewDefaults(DkImageView*, const DkImage*)` L595
- `dkImageDescriptorInitialize(DkImageDescriptor*, const DkImageView*, bool loadOrStore, bool decayMS)` L1346
- `DkCopyBuf { DkGpuAddr addr; uint32_t rowLength; uint32_t imageHeight; }` L1199
- `DkImageRect { x,y,z,width,height,depth }` L1190
- `dkCmdBufCopyBufferToImage(DkCmdBuf, const DkCopyBuf*, const DkImageView*, const DkImageRect*, uint32_t flags)` L1318 (flags 0)
- `dkCmdBufBarrier(DkCmdBuf, DkBarrier mode, uint32_t invalidateFlags)` L1259 — use `DkBarrier_Full` (L339) and `DkInvalidateFlags_Image` (L344)
- formats: `DkImageFormat_RGBA8_Unorm` L412; `DkImageType_2D` L355; `DkMsMode_1x` L530
- `DK_IMAGE_DESCRIPTOR_ALIGNMENT` (already used in dk_device.cpp)

## Rules
- Only dk_*.cpp includes <deko3d.h>. Log `[dk]`, one line. STOP rule as always
  (last 5 lines only, no self-fixing).

## Edits
1. `game/graphics/pipelines/dk_device.cpp` — descriptor growth:
   `constexpr uint32_t kMaxTexImages = 2048;`
   `kDescBytes`: `0x100` → `0x100 + kMaxTexImages * DK_IMAGE_DESCRIPTOR_ALIGNMENT` (= 0x40100).
   Splash keeps the first 0x100; image-descriptor slot i lives at
   `kOffDesc + 0x100 + i * DK_IMAGE_DESCRIPTOR_ALIGNMENT`. kOffUbo/kDataPool shift
   automatically (kOffUbo becomes 0x624100 — still 0x100-aligned, static_asserts hold).
   Update the layout comment.
2. NEW `game/graphics/deko3d_renderer/dk_texture_gpu.h` (plain: cstdint +
   dk_texture_pool.h only). Namespace `dktexgpu`, free functions:
   `bool init(); void teardown(); bool active();`
   `uint32_t upload_rgba8(uint32_t w, uint32_t h, const void* src);  // 0 = failed, caller skips upload (FIX 33)`
   `bool add_level_ref(uint32_t id); void release(uint32_t id);`
   `uint32_t live_count(); uint32_t high_water(); int failed_allocs(); uint64_t image_bytes_used();`
3. NEW `game/graphics/deko3d_renderer/dk_texture_gpu.cpp`: `#if defined(__SWITCH__)`,
   includes `<deko3d.h>`, `game/graphics/pipelines/dk_device_internal.h` (T22a),
   both dk_texture headers. File-static state: a 48 MB image memblock
   (`DkMemBlockMaker` + `DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image`, device from
   `dkdev::device_handle()`, size already 0x1000-aligned), a bump cursor, and a
   `dktex::TextureStore` (capacity `kMaxTexImages` via init(2048); init(0) invalid ids).
   upload_rgba8 flow:
   a. store alloc (`dktex::Entry{w,h,1,Fmt::Rgba8,level_refs=1}`) → slot; 0 → return 0.
   b. layout maker (defaults; `type=DkImageType_2D`, `dimensions={w,h,1}`, `mipLevels=1`,
      `format=DkImageFormat_RGBA8_Unorm`, `flags=0`) → size = GetSize, align = GetAlignment.
   c. round the bump cursor up to `align`; if `size` doesn't fit the remaining bytes →
      log `[dk] tex pool full (image %llu MB used)`, store-release the id, return 0.
   d. `dkImageInitialize` at that offset; `dkImageViewDefaults`;
      `dkImageDescriptorInitialize(&desc, &view, false, false)`; memcpy the descriptor to
      `dkdev::data_cpu() + kOffDesc + 0x100 + slot*DK_IMAGE_DESCRIPTOR_ALIGNMENT`
      (kOffDesc: expose via the internal header if not already — add `uint32_t desc_off()`
      and `uint32_t desc_slot_stride()` there ONLY if needed; prefer reading
      kMaxTexImages-aligned math locally).
   e. stage: `w*h*4` must be ≤ staging size (2 MB) — else fail like (c). memcpy to
      `dkdev::staging_cpu()`; then on `dkdev::cmd_handle()`:
      `dkCmdBufClear` → `dkCmdBufAddMemory(data memblock, dkdev::upload_cmd_off(), dkdev::upload_cmd_size())`
      → `dkCmdBufCopyBufferToImage` with `DkCopyBuf{dkdev::staging_gpu(), w*4, h}`,
      `DkImageRect{0,0,0,w,h,1}` → `dkCmdBufBarrier(cmd, DkBarrier_Full, DkInvalidateFlags_Image)`
      → `dkdev::submit_upload_cmds(dkCmdBufFinishList(cmd))`.
      (AddMemory needs the data DkMemBlock — expose `DkMemBlock data_mem()` via the
      internal header too, or reuse `staging_*` helpers; keep it one-liner.)
   f. log `[dk] tex id=%u %ux%u rgba8 %u KB`; return id.
   `teardown()` destroys the image memblock and re-zeros state; `active()` = memblock ok.
4. CMake: add dk_texture_gpu.cpp next to dk_renderer.cpp in the same Switch source list
   (find where dk_state.cpp was added and copy that entry).

## Done-check (background wrappers only)
`scripts/ai/check-bg.sh jak1` + `scripts/ai/check-tail.sh` → DONE-PASS.
Inert grep: `grep -rn 'dktexgpu::' game/ --include='*.cpp' | grep -v dk_texture_gpu` empty.
QWEN_REPORT `## T22b` (files + any API name you had to correct from the cheat-sheet +
last 5 log lines + md5). README row → 🟡. No commit, no deploy.
