# T22b — dk_texture_gpu: store + DkImage + RGBA8 upload (P2.3)

Second slice of the old T22 (needs T22a ✅). Creates the GPU texture half for RGBA8;
BCn is T22c. **Inert**: nothing outside calls dktexgpu yet.

## ALREADY ON DISK — do NOT redo (2026-10-17)
The 2026-10-17 00:53 session (reviewer-verified, see QWEN_REPORT "planner-typo
correction") already wrote:
- Edit 1: dk_device.cpp descriptor growth + layout comment (kDescBytes = 0x10100,
  kOffUbo 0x234100, kDataPool 0x2B4100 — static_asserts hold).
- Edit 2: dk_texture_gpu.h — the complete dktexgpu interface, exactly per spec.
- The dk_device_internal.h accessors `uint32_t desc_off()`,
  `uint32_t desc_slot_stride()`, `DkMemBlock data_mem()` and their one-liner
  definitions in dk_device.cpp — use them, do not re-add.
Your work = edit 0 (slot_of one-liner), edit 3's dk_texture_gpu.cpp (the .cpp ONLY — the
.h is done) and edit 4's CMake entry. If `dk_texture_gpu.cpp` already exists on disk,
STOP and report — it means
another session ran (2026-10-17 11:45: two concurrent sessions produced a hallucinated
report; one session per task, ever).

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
0. `game/graphics/deko3d_renderer/dk_texture_pool.h` (T19's plain, host-testable header —
   MAY be edited; keep it deko3d-free, no `__SWITCH__`): TextureStore keeps id→slot
   PRIVATE (m_id_to_slot), but the descriptor offset below needs the slot. Add ONE public
   one-liner next to get():
     `uint32_t slot_of(uint32_t id) const;  // live id -> slot; 0xFFFFFFFF if unknown/freed`
   Implementation: `auto it = m_id_to_slot.find(id); return it == m_id_to_slot.end() ? UINT32_MAX : it->second;`
1. DONE ON DISK — `game/graphics/pipelines/dk_device.cpp` descriptor growth (see above).
2. DONE ON DISK — `game/graphics/deko3d_renderer/dk_texture_gpu.h` (see above).
3. NEW `game/graphics/deko3d_renderer/dk_texture_gpu.cpp`: `#if defined(__SWITCH__)`,
   includes `<deko3d.h>`, `game/graphics/pipelines/dk_device_internal.h` (T22a),
   its own `dk_texture_gpu.h` (which already includes dk_texture_pool.h).

   MEMORY MAP — exactly this, no alternatives (the 12:24 session stalled 38 min deciding
   "which memblock / which offset"; this table is the answer, reviewer-verified 2026-10-17):
   | what                    | where exactly |
   |-------------------------|---------------|
   | every DkImage           | NEW 48 MB image memblock OWNED BY THIS FILE; offsets handed out by a local bump cursor starting at 0 |
   | upload pixel bytes      | memcpy → `dkdev::staging_cpu()` |
   | DkImageDescriptor bytes | memcpy → `dkdev::data_cpu() + dkdev::desc_off() + 0x100 + slot * dkdev::desc_slot_stride()` |
   | upload cmd recording    | `dkCmdBufClear(dkdev::cmd_handle())` then `dkCmdBufAddMemory(dkdev::cmd_handle(), dkdev::data_mem(), dkdev::upload_cmd_off(), dkdev::upload_cmd_size())` |
   | submission              | `dkdev::submit_upload_cmds(dkCmdBufFinishList(dkdev::cmd_handle()))` |
   NOT dkdev::buffer_pool() (that is T21's 8 MB CPU-side vertex/index pool — the stuck
   session fixated on it), NOT s_dataMem, NOT dk_device's make_block().

   File-static state (exactly these):
   - `constexpr uint32_t kImgPoolBytes = 0x3000000;`  // 48 MB; multiple of DK_MEMBLOCK_ALIGNMENT (0x1000, deko3d.h:140)
   - `DkMemBlock s_imgMem = nullptr;`  `uint64_t s_cursor = 0;`  `int s_bumpFails = 0;`
   - `dktex::TextureStore s_store;`
   - `DkImage s_images[2048];`  // DkImage = 128-B opaque VALUE struct (DK_DECL_OPAQUE, deko3d.h:82); index = store slot; NO map, NO new/delete

   init() body, exactly:
     `DkMemBlockMaker mm;`
     `dkMemBlockMakerDefaults(&mm, dkdev::device_handle(), kImgPoolBytes);`  // deko3d.h:193
     `mm.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;`        // override the CpuUncached|GpuCached default (deko3d.h:197)
     `s_imgMem = dkMemBlockCreate(&mm);`  // deko3d.h:1233; nullptr → log `[dk] tex img memblock 48 MB failed`, return false
     `if (!s_store.init(2048)) { dkMemBlockDestroy(s_imgMem); s_imgMem = nullptr; return false; }`
     `s_cursor = 0; return true;`
   teardown(): `if (s_imgMem) dkMemBlockDestroy(s_imgMem); s_imgMem = nullptr; s_store = {}; s_cursor = 0;`
   active(): `return s_imgMem != nullptr;`

   upload_rgba8(w, h, src), steps in order ("release+return 0" below means
   `s_store.release(id); return 0;`):
   a. `uint32_t id = s_store.alloc(dktex::Entry{ (uint16_t)w, (uint16_t)h, 1, dktex::Fmt::Rgba8, 1 });`
      id == 0 → return 0 (the store already counted its failed_allocs).
   b. `uint32_t slot = s_store.slot_of(id);`  // edit 0; id is live here, so slot < 2048
   c. locals `DkImageLayoutMaker mk; DkImageLayout layout;` then
      `dkImageLayoutMakerDefaults(&mk, dkdev::device_handle());`
      `mk.type = DkImageType_2D; mk.dimensions[0] = w; mk.dimensions[1] = h; mk.dimensions[2] = 1;`
      `mk.mipLevels = 1; mk.format = DkImageFormat_RGBA8_Unorm; mk.flags = 0;`
      `dkImageLayoutInitialize(&layout, &mk);`
      `const uint64_t size = dkImageLayoutGetSize(&layout);`
      `const uint32_t align_ = dkImageLayoutGetAlignment(&layout);`
   d. guards: `uint64_t off = (s_cursor + align_ - 1) / align_ * align_;`
      `off + size > kImgPoolBytes` → `s_bumpFails++`, log `[dk] tex pool full (%llu MB used)`
      (s_cursor/1048576), release+return 0;
      `(uint64_t)w * h * 4 > 512 * 1024 * 4` → log `[dk] tex too big for staging (%ux%u)`,
      release+return 0. (512*1024*4 = the staging area size, dk_device.cpp kStagingMax.)
   e. `dkImageInitialize(&s_images[slot], &layout, s_imgMem, (uint32_t)off); s_cursor = off + size;`
      `DkImageView view; dkImageViewDefaults(&view, &s_images[slot]);`
      `DkImageDescriptor desc; dkImageDescriptorInitialize(&desc, &view, false, false);`
      `memcpy(dkdev::data_cpu() + dkdev::desc_off() + 0x100 + slot * dkdev::desc_slot_stride(), &desc, sizeof(desc));`
   f. `memcpy(dkdev::staging_cpu(), src, (size_t)w * h * 4);`
      `DkCmdBuf cmd = dkdev::cmd_handle();`
      `dkCmdBufClear(cmd);`
      `dkCmdBufAddMemory(cmd, dkdev::data_mem(), dkdev::upload_cmd_off(), dkdev::upload_cmd_size());`
      `const DkCopyBuf cp = { dkdev::staging_gpu(), w * 4, h };`  // rowLength = bytes per row
      `const DkImageRect rc = { 0, 0, 0, w, h, 1 };`
      `dkCmdBufCopyBufferToImage(cmd, &cp, &view, &rc, 0);`
      `dkCmdBufBarrier(cmd, DkBarrier_Full, DkInvalidateFlags_Image);`
      `dkdev::submit_upload_cmds(dkCmdBufFinishList(cmd));`
   g. log `[dk] tex id=%u slot=%u %ux%u rgba8 %u KB`; return id.

   Forwarders: add_level_ref(id)/release(id) → s_store.add_level_ref/release (release
   does NOT rewind s_cursor and does NOT touch s_images — freed slots are simply reused
   by the store; image memory dies only with the memblock in teardown()).
   live_count()/high_water() → s_store; failed_allocs() = s_store.failed_allocs() +
   s_bumpFails; image_bytes_used() = s_cursor.
4. CMake: add dk_texture_gpu.cpp next to dk_renderer.cpp in the same Switch source list
   (game/CMakeLists.txt:101, the dk_state.cpp entry — copy that line).

## Done-check (background wrappers only)
`scripts/ai/check-bg.sh jak1` + `scripts/ai/check-tail.sh` → DONE-PASS.
Inert grep: `grep -rn 'dktexgpu::' game/ --include='*.cpp' | grep -v dk_texture_gpu` empty.
QWEN_REPORT `## T22b` (files + any API name you had to correct from the cheat-sheet +
last 5 log lines + md5). README row → 🟡. No commit, no deploy.
