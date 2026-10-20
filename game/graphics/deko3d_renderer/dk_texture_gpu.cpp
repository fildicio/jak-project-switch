/*!
 * @file dk_texture_gpu.cpp
 * deko3d migration Phase 2.3 (T22b, AI-assisted): the GPU half of the P2.3
 * texture pool. This is the only TU that ties dktex::TextureStore (the bookkeeping
 * half, T19/`dk_texture_pool.h`) to the real deko3d image machinery: it owns a
 * single 48 MB deko3d image memblock, carves DkImage handles out of it with a
 * bump cursor, writes each texture's DkImageDescriptor into the data memblock at
 * (dkdev::desc_off() + 0x100 + slot * dkdev::desc_slot_stride()), and uploads
 * RGBA8 textures through the shared staging buffer + the dedicated upload cmd
 * region via submit_upload_cmds().
 *
 * Inert: nothing outside this file calls dktexgpu::* yet (the first consumer is
 * T23, the TexturePool seam). So the file is wrapped in
 * `#if defined(__SWITCH__)` exactly like dk_device.cpp -- only a deko3d TU may
 * include <deko3d.h>, and the public interface in dk_texture_gpu.h stays plain
 * (host-testable, no deko3d).
 *
 * Memory map (reviewer-verified 2026-10-17, see the T22b task file):
 *   - every DkImage          -> NEW 48 MB image memblock OWNED BY THIS FILE
 *   - upload pixel bytes     -> memcpy -> dkdev::staging_cpu()
 *   - DkImageDescriptor bytes -> memcpy -> dkdev::data_cpu() + dkdev::desc_off()
 *                                + 0x100 + slot * dkdev::desc_slot_stride()
 *   - upload cmd recording   -> dkCmdBufClear + dkCmdBufAddMemory(upCmd region)
 *   - submission              -> dkdev::submit_upload_cmds(dkCmdBufFinishList(cmd))
 */

#if defined(__SWITCH__)

#include "game/graphics/deko3d_renderer/dk_texture_gpu.h"

#include <deko3d.h>
#include <string.h>
#include <atomic>  // T47: one-shot log flag shared across threads
#include <thread>  // T47: std::this_thread::get_id for the owner-thread guard
#include "game/graphics/pipelines/dk_device_internal.h"  // T22a: dk-TU internals pass-throughs

// switch_run_logf lives in run_log.h, which cannot be included next to <switch.h>
// (u128 collision). Same hand declaration as switch/platform.cpp / dk_device.cpp.
void switch_run_logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

namespace dktexgpu {

// 48 MB of image memblock, all 0x1000-aligned. 0x3000000 is a DK_MEMBLOCK_ALIGNMENT
// (0x1000; deko3d.h:140) multiple, so no rounding is needed.
constexpr uint32_t kImgPoolBytes = 0x3000000;  // 48 MB

// 2048 slots; index = the dktex::TextureStore slot. DkImage is a 128-B opaque VALUE
// struct (DK_DECL_OPAQUE, deko3d.h:82), so a plain C array (no map, no new/delete)
// is the right shape.
static DkMemBlock      s_imgMem  = nullptr;
static uint64_t        s_cursor  = 0;
static int             s_bumpFails = 0;
static dktex::TextureStore s_store;
static DkImage         s_images[2048];  // [slot]; index = store slot

// T47 (AI-assisted): the uploads below record into dkdev's ONE cmd buffer and submit on
// its ONE queue, which present_frame() also uses; deko3d cmd buffers and queues are not
// thread-safe, and s_store has no lock. So only the thread that ran init() (the render
// thread, T48) may touch them. Any other thread is refused: uploads return 0 (the FIX 33
// "skip this texture" contract), ref changes are dropped (a leak, never a corruption).
static std::thread::id s_owner;
static std::atomic<bool> s_foreign_logged{false};

static bool on_owner_thread(const char* what) {
  if (std::this_thread::get_id() == s_owner) {
    return true;
  }
  if (!s_foreign_logged.exchange(true)) {
    switch_run_logf("[dk] tex %s called off the render thread: refused (T47 guard)", what);
  }
  return false;
}

bool init() {
  DkMemBlockMaker mm;
  dkMemBlockMakerDefaults(&mm, dkdev::device_handle(), kImgPoolBytes);
  mm.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;  // override the CpuUncached|GpuCached default
  s_imgMem = dkMemBlockCreate(&mm);
  if (s_imgMem == nullptr) {
    switch_run_logf("[dk] tex img memblock 48 MB failed");
    return false;
  }
  if (!s_store.init(2048)) {
    dkMemBlockDestroy(s_imgMem);
    s_imgMem = nullptr;
    return false;
  }
  s_cursor = 0;
  s_owner = std::this_thread::get_id();  // T47: the init thread owns all uploads
  return true;
}

void teardown() {
  if (s_imgMem) {
    dkMemBlockDestroy(s_imgMem);
  }
  s_imgMem = nullptr;
  s_store = {};
  s_cursor = 0;
  s_owner = std::thread::id();  // T47: no owner until the next init()
}

bool active() {
  return s_imgMem != nullptr;
}

uint32_t upload_rgba8(uint32_t w, uint32_t h, const void* src) {
  if (!on_owner_thread("upload_rgba8")) {
    return 0;
  }
  // (a) bookkeeping: take a live id from the store (it owns the slot lifetime).
  uint32_t id = s_store.alloc(
      dktex::Entry{ (uint16_t)w, (uint16_t)h, 1, dktex::Fmt::Rgba8, 1 });
  if (id == 0) {
    return 0;  // store already counted the failed_allocs
  }
  // (b) slot for the descriptor + DkImage (id is live, so slot < 2048).
  uint32_t slot = s_store.slot_of(id);

  // (c) image layout for a 1-mip RGBA8 2D texture.
  DkImageLayoutMaker mk;
  DkImageLayout layout;
  dkImageLayoutMakerDefaults(&mk, dkdev::device_handle());
  mk.type         = DkImageType_2D;
  mk.dimensions[0] = w;
  mk.dimensions[1] = h;
  mk.dimensions[2] = 1;
  mk.mipLevels     = 1;
  mk.format        = DkImageFormat_RGBA8_Unorm;
  mk.flags         = 0;
  dkImageLayoutInitialize(&layout, &mk);
  const uint64_t  size   = dkImageLayoutGetSize(&layout);
  const uint32_t  align_ = dkImageLayoutGetAlignment(&layout);

  // (d) bump-cursor + alignment, with the two failure guards from the spec.
  const uint64_t off = (s_cursor + align_ - 1) / align_ * align_;
  if (off + size > kImgPoolBytes) {
    s_bumpFails++;
    switch_run_logf("[dk] tex pool full (%llu MB used)",
                    (unsigned long long)(s_cursor / 1048576));
    s_store.release(id);  // release+return 0
    return 0;
  }
  if ((uint64_t)w * h * 4 > 512 * 1024 * 4) {  // 512*1024*4 = kStagingMax
    switch_run_logf("[dk] tex too big for staging (%ux%u)", w, h);
    s_store.release(id);  // release+return 0
    return 0;
  }

  // (e) carve the DkImage at `off` and write its descriptor into the data memblock.
  dkImageInitialize(&s_images[slot], &layout, s_imgMem, (uint32_t)off);
  s_cursor = off + size;
  DkImageView view;
  dkImageViewDefaults(&view, &s_images[slot]);
  DkImageDescriptor desc;
  dkImageDescriptorInitialize(&desc, &view, false, false);
  memcpy(dkdev::data_cpu() + dkdev::desc_off() + 0x100 + slot * dkdev::desc_slot_stride(),
         &desc, sizeof(desc));

  // (f) push the pixels through the shared staging buffer + the upload cmd region.
  memcpy(dkdev::staging_cpu(), src, (size_t)w * h * 4);
  DkCmdBuf cmd = dkdev::cmd_handle();
  dkCmdBufClear(cmd);
  dkCmdBufAddMemory(cmd, dkdev::data_mem(), dkdev::upload_cmd_off(),
                    dkdev::upload_cmd_size());
  // T51: {addr, 0, 0} = tightly packed, exactly the splash upload that works on hardware
  // (dk_device.cpp upload_splash). imageHeight is a LAYER STRIDE in bytes, not a row count
  // (deko3d dk_image.cpp L738); the old {w*4, h} passed 16 B for a 16x16 image.
  // No barrier here either (splash has none); P3 must invalidate the texture cache
  // (dkCmdBufBarrier(..., DkInvalidateFlags_Image)) in the frame list before sampling.
  const DkCopyBuf  cp = { dkdev::staging_gpu(), 0, 0 };
  const DkImageRect rc = { 0, 0, 0, w, h, 1 };
  dkCmdBufCopyBufferToImage(cmd, &cp, &view, &rc, 0);
  if (!dkdev::submit_upload_cmds(dkCmdBufFinishList(cmd))) {
    s_store.release(id);  // T51: GPU did not finish the copy -> "skip", never hang
    return 0;
  }

  // (g) done.
  switch_run_logf("[dk] tex id=%u slot=%u %ux%u rgba8 %u KB", id, slot, w, h,
                  (unsigned)((uint64_t)w * h * 4 / 1024));
  return id;
}

uint32_t upload_bcn(dktex::Fmt fmt, uint32_t w, uint32_t h, const void* src,
                    uint32_t size) {
  if (!on_owner_thread("upload_bcn")) {
    return 0;
  }
  // (a) format map — only BCn is valid here. Rgba8 must go through upload_rgba8;
  // reject it early (before touching the store) so no slot is wasted.
  DkImageFormat layoutFmt;
  uint16_t blockBytes;  // bytes per compressed block
  switch (fmt) {
    case dktex::Fmt::Bc1:
      layoutFmt  = DkImageFormat_RGB_BC1;
      blockBytes = 8;
      break;
    case dktex::Fmt::Bc2:
      layoutFmt  = DkImageFormat_RGBA_BC2;
      blockBytes = 16;
      break;
    case dktex::Fmt::Bc3:
      layoutFmt  = DkImageFormat_RGBA_BC3;
      blockBytes = 16;
      break;
    case dktex::Fmt::Bc7:
      layoutFmt  = DkImageFormat_RGBA_BC7_Unorm;
      blockBytes = 16;
      break;
    case dktex::Fmt::Rgba8:
      switch_run_logf("[dk] tex bcn fmt unsupported (rgba8)");
      return 0;
    default:
      switch_run_logf("[dk] tex bcn fmt unsupported (fmt=%u)", (unsigned)fmt);
      return 0;
  }

  // (b) bookkeeping: take a live id from the store (it owns the slot lifetime).
  uint32_t id = s_store.alloc(
      dktex::Entry{ (uint16_t)w, (uint16_t)h, 1, fmt, 1 });
  if (id == 0) {
    return 0;  // store already counted the failed_allocs
  }
  // (c) slot for the descriptor + DkImage (id is live, so slot < 2048).
  uint32_t slot = s_store.slot_of(id);

  // (d) payload sanity: blocks = ceil(w/4)*ceil(h/4); block bytes 8 (BC1) / 16
  // (BC2/BC3/BC7). EXPECT is informational only -- a mismatch only WARN-logs; the
  // caller's `size` wins (it knows the exact byte layout of the card payload).
  const uint32_t blocksX    = (w + 3) / 4;
  const uint32_t blocksY    = (h + 3) / 4;
  const uint64_t expectBytes = (uint64_t)blocksX * blocksY * blockBytes;
  if ((uint64_t)size != expectBytes) {
    switch_run_logf("[dk] tex bcn size mismatch: got %u want %lu (%ux%u block=%u)",
                    size, (unsigned long)expectBytes, w, h, (unsigned)blockBytes);
  }

  // (e) image layout for a 1-mip 2D texture in the mapped BCn format.
  DkImageLayoutMaker mk;
  DkImageLayout layout;
  dkImageLayoutMakerDefaults(&mk, dkdev::device_handle());
  mk.type         = DkImageType_2D;
  mk.dimensions[0] = w;
  mk.dimensions[1] = h;
  mk.dimensions[2] = 1;
  mk.mipLevels     = 1;
  mk.format        = layoutFmt;
  mk.flags         = 0;
  dkImageLayoutInitialize(&layout, &mk);
  const uint64_t  size_  = dkImageLayoutGetSize(&layout);
  const uint32_t  align_ = dkImageLayoutGetAlignment(&layout);

  // (f) bump-cursor + alignment, with the two failure guards from the spec.
  const uint64_t off = (s_cursor + align_ - 1) / align_ * align_;
  if (off + size_ > kImgPoolBytes) {
    s_bumpFails++;
    switch_run_logf("[dk] tex pool full (%llu MB used)",
                    (unsigned long long)(s_cursor / 1048576));
    s_store.release(id);  // release+return 0
    return 0;
  }
  if ((uint64_t)size > 512 * 1024 * 4) {  // 512*1024*4 = kStagingMax
    switch_run_logf("[dk] tex too big for staging (%u bytes)", size);
    s_store.release(id);  // release+return 0
    return 0;
  }

  // (g) carve the DkImage at `off` and write its descriptor into the data memblock.
  dkImageInitialize(&s_images[slot], &layout, s_imgMem, (uint32_t)off);
  s_cursor = off + size_;
  DkImageView view;
  dkImageViewDefaults(&view, &s_images[slot]);
  DkImageDescriptor desc;
  dkImageDescriptorInitialize(&desc, &view, false, false);
  memcpy(dkdev::data_cpu() + dkdev::desc_off() + 0x100 + slot * dkdev::desc_slot_stride(),
         &desc, sizeof(desc));

  // (h) push the payload through the shared staging buffer + the upload cmd region.
  // BCn is already tightly packed on the card, so DkCopyBuf / DkImageRect use the
  // "derive the pitch from the image layout" form (rowLength = 0, imageHeight = 0),
  // exactly as the only vendored BCn upload (SampleFramework/CExternalImage.cpp,
  // Example04 cat-256x256.bc1). Do NOT compute row pitches.
  memcpy(dkdev::staging_cpu(), src, size);
  DkCmdBuf cmd = dkdev::cmd_handle();
  dkCmdBufClear(cmd);
  dkCmdBufAddMemory(cmd, dkdev::data_mem(), dkdev::upload_cmd_off(),
                    dkdev::upload_cmd_size());
  const DkCopyBuf  cp = { dkdev::staging_gpu(), 0, 0 };  // tightly packed
  const DkImageRect rc = { 0, 0, 0, w, h, 1 };           // w,h in PIXELS
  dkCmdBufCopyBufferToImage(cmd, &cp, &view, &rc, 0);
  if (!dkdev::submit_upload_cmds(dkCmdBufFinishList(cmd))) {  // T51: same as upload_rgba8
    s_store.release(id);
    return 0;
  }

  // (i) done.
  switch_run_logf("[dk] tex id=%u slot=%u %ux%u bcn(%u) %u KB", id, slot, w, h,
                  (unsigned)fmt, (unsigned)(size / 1024));
  return id;
}

bool add_level_ref(uint32_t id) {
  if (!on_owner_thread("add_level_ref")) {
    return false;
  }
  return s_store.add_level_ref(id);
}

void release(uint32_t id) {
  if (!on_owner_thread("release")) {
    return;
  }
  // s_store.release frees only the dktex slot; it does NOT rewind s_cursor and does
  // NOT touch s_images. Image memory dies only when the memblock is destroyed in
  // teardown(); freed slots are simply reused by the store on the next alloc().
  s_store.release(id);
}

uint32_t slot_of(uint32_t id) {
  return id == 0 ? UINT32_MAX : s_store.slot_of(id);
}

// T82 (AI-assisted): an RGBA8 texture that is also a colour render target (eye textures).
// Same bump carve + descriptor as upload_rgba8, no pixel upload; the contents are undefined
// until the first draw. Never released (the eye targets live for the whole run).
uint32_t create_render_target(uint32_t w, uint32_t h) {
  if (!on_owner_thread("create_render_target")) {
    return 0;
  }
  uint32_t id = s_store.alloc(dktex::Entry{(uint16_t)w, (uint16_t)h, 1, dktex::Fmt::Rgba8, 1});
  if (id == 0) {
    return 0;
  }
  const uint32_t slot = s_store.slot_of(id);
  DkImageLayoutMaker mk;
  DkImageLayout layout;
  dkImageLayoutMakerDefaults(&mk, dkdev::device_handle());
  mk.type = DkImageType_2D;
  mk.dimensions[0] = w;
  mk.dimensions[1] = h;
  mk.dimensions[2] = 1;
  mk.mipLevels = 1;
  mk.format = DkImageFormat_RGBA8_Unorm;
  mk.flags = DkImageFlags_UsageRender;
  dkImageLayoutInitialize(&layout, &mk);
  const uint64_t size = dkImageLayoutGetSize(&layout);
  const uint32_t align_ = dkImageLayoutGetAlignment(&layout);
  const uint64_t off = (s_cursor + align_ - 1) / align_ * align_;
  if (off + size > kImgPoolBytes) {
    s_bumpFails++;
    s_store.release(id);
    return 0;
  }
  dkImageInitialize(&s_images[slot], &layout, s_imgMem, (uint32_t)off);
  s_cursor = off + size;
  DkImageView view;
  dkImageViewDefaults(&view, &s_images[slot]);
  DkImageDescriptor desc;
  dkImageDescriptorInitialize(&desc, &view, false, false);
  memcpy(dkdev::data_cpu() + dkdev::desc_off() + 0x100 + slot * dkdev::desc_slot_stride(),
         &desc, sizeof(desc));
  return id;
}

const void* image_of(uint32_t id) {
  const uint32_t slot = slot_of(id);
  return slot == UINT32_MAX ? nullptr : &s_images[slot];
}

// T60 (AI-assisted): the tfrag time-of-day palettes change every few frames. A new
// upload_rgba8 each time would leak image memory (release() never rewinds s_cursor),
// so the palette texture is created once and its texels are rewritten here.
bool update_rgba8(uint32_t id, uint32_t w, uint32_t h, const void* src) {
  if (!on_owner_thread("update_rgba8")) {
    return false;
  }
  dktex::Entry* e = s_store.get(id);
  const uint32_t slot = s_store.slot_of(id);
  if (e == nullptr || slot == UINT32_MAX || e->fmt != dktex::Fmt::Rgba8 || w == 0 || h == 0 ||
      w > e->w || h > e->h || (uint64_t)w * h * 4 > 512 * 1024 * 4) {
    return false;
  }
  DkImageView view;
  dkImageViewDefaults(&view, &s_images[slot]);
  memcpy(dkdev::staging_cpu(), src, (size_t)w * h * 4);
  DkCmdBuf cmd = dkdev::cmd_handle();
  dkCmdBufClear(cmd);
  dkCmdBufAddMemory(cmd, dkdev::data_mem(), dkdev::upload_cmd_off(), dkdev::upload_cmd_size());
  const DkCopyBuf cp = {dkdev::staging_gpu(), 0, 0};  // tightly packed, as upload_rgba8
  const DkImageRect rc = {0, 0, 0, w, h, 1};
  dkCmdBufCopyBufferToImage(cmd, &cp, &view, &rc, 0);
  return dkdev::submit_upload_cmds(dkCmdBufFinishList(cmd));
}

uint32_t live_count()       { return s_store.live_count(); }
uint32_t high_water()       { return s_store.high_water(); }
int      failed_allocs()    { return s_store.failed_allocs() + s_bumpFails; }
uint64_t image_bytes_used() { return s_cursor; }

}  // namespace dktexgpu

#endif  // __SWITCH__

