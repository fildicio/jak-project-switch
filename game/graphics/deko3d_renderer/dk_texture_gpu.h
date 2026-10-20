#pragma once
// deko3d P2.3 texture pool, GPU half (AI-assisted, T22b). The "GPU texture" of
// the planned TexturePoolDeko3D that T19 (dk_texture_pool.h) sketched on the
// deko3d side: it OWNS a fixed 48 MB deko3d image memblock, hands out live
// DkImage handles via a bump cursor, and uploads RGBA8 (uncompressed) textures
// through a single staging buffer + the dedicated upload command region.
//
// T19 (dk_texture_pool.h / dktex::TextureStore) is the bookkeeping half (id/slot
// ref-counting, FIX 33 dedup); this file is the GPU half that actually creates the
// DkImage + binds the descriptor slot into the data memblock and pushes pixels.
// The two are composed here: every upload is a (store alloc, DkImage carve, upload)
// triple, and every release is a (store release) that lets the slot be reused.
//
// This header stays PLAIN (no <deko3d.h>, no __SWITCH__ guard) so it can be
// host-tested like dk_texture_pool.h: only cstdint + dk_texture_pool.h. The
// deko3d-dependent implementation lives in dk_texture_gpu.cpp (T22b), which is
// itself wrapped in `#if defined(__SWITCH__)` — the same split as dk_device.h
// (public interface) vs dk_device.cpp (deko3d impl).
//
// Inert: no caller outside dk_texture_gpu.cpp invokes dktexgpu::* yet. The first
// consumer is T23 (the TexturePool seam behind the kill-switch), which wires the
// GOAL texture-load path through here.
//
// Caller contract (FIX 33, same as dkpool::BufferPool): when upload_rgba8()
// returns 0 the caller MUST skip the upload; the failed allocation is already
// logged at the source (the store and/or the memblock bump), so the caller does
// not need to log again.
//
// Not thread-safe: render-thread only (matches dktex::TextureStore).

#include <cstdint>

#include "game/graphics/deko3d_renderer/dk_texture_pool.h"

namespace dktexgpu {

// 1 = invalid / failed-acquire sentinel. dktex::TextureStore already reserves id 1
// (kPlaceholderId) like the GL placeholder texture, so the first real id handed
// out by init() is 2; this header does not re-reserve it.

// Size the P2.3 texture GPU half:
//   - alloc the 48 MB image memblock (DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
//   - init the dktex::TextureStore at capacity = kMaxTexImages (2048 slots);
//   - reset the bump cursor and the per-tex DkImage handle map.
// False when the memblock does not allocate or the store fails to init (the
// memblock is destroyed if the store init fails, and active() stays false).
bool init();

// Free the image memblock (if any) and re-zero all state. Safe to call when
// init() never ran or failed (active() == false): it is a no-op then.
void teardown();

// True iff init() completed and has not been torn down — i.e. the image memblock
// is live and ready to carve textures from.
bool active();

// Upload one uncompressed RGBA8 (unorm) texture.
//   w, h     image width and height in pixels (no mip chain, single level)
//   src      CPU pointer to w*h*4 bytes of RGBA8 data (4 bytes per pixel, no stride)
// Returns the new texture id (>= 2, monotonic, never reused) on success, or 0
// on failure (store full / memblock bump fails / staging too small for this
// texture / any deko3d call fails), in which case the caller MUST skip this
// texture (FIX 33 caller contract). On every failure the dktex::TextureStore
// slot that was just allocated is released back, so no leak.
//
// On success the texture lives at its own DkImage in the image memblock, with
// its DkImageDescriptor memcpy'd into the data memblock at
// (dkdev::desc_off() + 0x100 + slot * dkdev::desc_slot_stride()). The slot is
// the dktex::TextureStore slot index (not the id); the caller later reads the
// texture with the descriptor's id (which is what the store hands back).
uint32_t upload_rgba8(uint32_t w, uint32_t h, const void* src);

// Upload one compressed (BCn) texture directly from the card bytes (zero CPU decode).
//   fmt      dktex::Fmt::Bc1 / Bc2 / Bc3 / Bc7 (Rgba8 → reject; use upload_rgba8)
//   w, h     image width and height in pixels (no mip chain, single level)
//   src      CPU pointer to the BCn payload (blocks, tightly packed)
//   size     number of bytes in the payload (the caller knows the exact block count)
// Returns the new texture id (>= 2) on success, or 0 on failure (same contract
// as upload_rgba8). The BlockSize (8 for BC1, 16 for BC2/BC3/BC7) and expected
// byte count (ceil(w/4)*ceil(h/4)*blockBytes) are sanity-checked; a mismatch only
// WARN-logs (the caller's `size` wins).
uint32_t upload_bcn(dktex::Fmt fmt, uint32_t w, uint32_t h, const void* src, uint32_t size);

// Bump the level ref-count of a live texture (the same "second level loads the
// same tex without re-uploading" semantics as dktex::TextureStore::add_level_ref).
// False on a bad / unknown / already-freed id.
bool add_level_ref(uint32_t id);

// Drop one level ref of a live texture. When the count hits 0 the DkImage is
// released (the bump cursor is NOT rewound — freed ids are never reused, only
// their dktex::TextureStore slot can be re-cared); the id is dead forever and
// cannot alias a still-live texture. Id 0 / unknown / double-release are no-ops
// (defensive, never a second free / an underflow). False on a bad id.
void release(uint32_t id);

// T60 (AI-assisted): descriptor slot of a live id (the image index for
// dkMakeTextureHandle), or UINT32_MAX for 0 / unknown / freed ids.
uint32_t slot_of(uint32_t id);

// T82 (AI-assisted): w x h RGBA8 image usable as a colour render target AND a sampled
// texture (eye textures). Returns the id (0 = failed). Render thread only.
uint32_t create_render_target(uint32_t w, uint32_t h);
// T82: the DkImage* of a live id (as const void*, this header stays deko3d-free), or nullptr.
const void* image_of(uint32_t id);

// T60: overwrite the top-left w x h texels of a live RGBA8 texture (same staging
// path as upload_rgba8, no new image memory). False on a bad id, a rect bigger
// than the texture, or a failed upload. Render thread only.
bool update_rgba8(uint32_t id, uint32_t w, uint32_t h, const void* src);

// T86 (AI-assisted): same as update_rgba8 but never waits for the GPU. The texels go into
// a 3-slot staging ring; record_deferred_updates() (inside the world recorder, before any
// draw) records the copies into the frame command buffer. Call begin_deferred_frame()
// once at the start of every build. Render thread only.
void begin_deferred_frame();
bool update_rgba8_deferred(uint32_t id, uint32_t w, uint32_t h, const void* src);
uint32_t record_deferred_updates();  // returns the number of copies recorded
uint64_t deferred_total();           // copies recorded since boot
uint64_t deferred_sync_fallbacks();  // updates that fell back to the waiting path

// Telemetry — mirror the dktex::TextureStore accessors plus the GPU-side bytes used.
uint32_t live_count();       // textures live right now
uint32_t high_water();       // max simultaneous live texts ever reached
int failed_allocs();         // store-alloc + carve failures
uint64_t image_bytes_used(); // bump cursor (bytes of the 48 MB pool carved so far)

}  // namespace dktexgpu