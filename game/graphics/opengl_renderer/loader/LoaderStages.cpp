#include <algorithm>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <unistd.h>

#include "LoaderStages.h"

#include "Loader.h"

#include "common/global_profiler/GlobalProfiler.h"

/*!
 * Bind + glBufferSubData, skipped entirely when the level loader could not get
 * GPU storage for the buffer (GpuBufferPool::acquire returned 0).
 *
 * Uploading into such a buffer is not merely a no-op: a buffer object whose
 * glBufferData failed still reports the requested size, so the size validation
 * inside glBufferSubData passes and the driver goes on to map storage that
 * does not exist. On Mesa/nouveau that map is NULL and the upload becomes a
 * memcpy into low memory, which is the jak2 level-transition crash (data abort
 * writing to NULL + chunk offset).
 */
static void upload_to_buffer(GLenum target,
                             GLuint buffer,
                             GLintptr offset,
                             GLsizeiptr size,
                             const void* data) {
  if (buffer == 0) {
    return;
  }
  glBindBuffer(target, buffer);
  glBufferSubData(target, offset, size, data);
}

// ---------------------------------------------------------------------------
// Per-frame loader budget and upload chunk sizes.
//
// Desktop: generous values stream a level in over a handful of frames.
// Switch (Tegra X1): the budget is only checked *between* uploads, and a full
// 32768-vertex chunk is exactly 1 MB of PreloadedVertex - a single
// glBufferSubData of that size can stall the frame for 20+ ms (FIX 9 in
// SWITCH_PORT_SESSION_NOTES.md). Use smaller chunks and a tighter time budget
// there; levels stream in over more frames instead of hitching.
// ---------------------------------------------------------------------------
#ifdef __SWITCH__
// FIX 36 Task 3 (AI-assisted): runtime budget, retuned every frame by
// Loader::update_frame_budget() (blackout vs healthy vs struggling frame
// rate). The old flat constants are now just the "streaming" fallback.
LoaderFrameBudget g_loader_budget = {2.f, 256 * 1024, 512};
constexpr u32 STAGE_VERT_CHUNK = 8192;       // verts (~256 KB for PreloadedVertex)
constexpr u32 STAGE_INDEX_CHUNK = 8192 * 8;  // u32 indices (~256 KB)
#else
LoaderFrameBudget g_loader_budget = {4.5f, 1024 * 1024, 2048};
constexpr u32 STAGE_VERT_CHUNK = 32768;       // verts (1 MB for PreloadedVertex)
constexpr u32 STAGE_INDEX_CHUNK = 32768 * 8;  // u32 indices (1 MB)
#endif
// FIX 39 LoadBoost (AI-assisted): see LoaderStages.h.
//
// FIX 46b: the enter/leave counts are wider. `kEnterFrames` was 3, which meant
// a load starting at 13 fps took 3 frames (~230 ms at that rate) to drop the
// resolution -- by which time the visible stall had already happened.
// `kLeaveFrames` is raised at the same time so the extra sensitivity cannot
// introduce resolution flicker.
namespace {
int g_loadboost_on_frames = 0;    // consecutive frames with a backlog
int g_loadboost_off_frames = 0;   // consecutive frames without one
bool g_loadboost_active = false;
constexpr int kEnterFrames = 2;   // ~0.07 s: react before the player sees a long stall
constexpr int kLeaveFrames = 60;  // ~2 s: never flicker the resolution
}  // namespace

void loadboost_set_streaming(bool streaming) {
  if (streaming) {
    g_loadboost_off_frames = 0;
    if (++g_loadboost_on_frames >= kEnterFrames) {
      g_loadboost_active = true;
    }
  } else {
    g_loadboost_on_frames = 0;
    if (++g_loadboost_off_frames >= kLeaveFrames) {
      g_loadboost_active = false;
    }
  }
}

bool loadboost_active() {
  return g_loadboost_active;
}

// FIX 42 deferred mipmaps (AI-assisted): see LoaderStages.h.
namespace {
std::vector<u32> g_mip_queue;
}  // namespace

void mipq_defer(u32 gl_texture) {
  g_mip_queue.push_back(gl_texture);
}

// FIX 49 (AI-assisted): last drain's rate/did, for the split-budget log line.
u32 g_last_mip_rate = 0;
u32 g_last_mip_did = 0;

// FIX 52 (AI-assisted): uploads submitted this frame; see the header comment.
u32 g_loader_gpu_submits_this_frame = 0;

size_t mipq_pending() {
  return g_mip_queue.size();
}

// FIX 100 (AI-assisted): an unloaded level's textures sit in the loader's garbage
// queue for seconds since FIX 99 time-boxed the drain, and glIsTexture() is still
// true for them, so mipq_process kept paying 3-5 ms glGenerateMipmap chains on
// textures nobody will ever draw. Once they are deleted, a recycled GL name could
// also make a stale entry touch the NEXT level's texture. Drop them at unload.
void mipq_forget(const std::vector<u32>& gl_textures) {
  if (g_mip_queue.empty() || gl_textures.empty()) {
    return;
  }
  std::unordered_set<u32> gone(gl_textures.begin(), gl_textures.end());
  std::erase_if(g_mip_queue, [&](u32 t) { return gone.count(t) > 0; });
}

int mipq_process(int max_count, float max_ms) {
  int done = 0;
  Timer mip_timer;
  mip_timer.start();
  glActiveTexture(GL_TEXTURE0);
  // FIX 94: max_ms is checked after each chain, so at least one always runs.
  while (done < max_count && !g_mip_queue.empty() && (done == 0 || mip_timer.getMs() < max_ms)) {
    const u32 tex = g_mip_queue.back();
    g_mip_queue.pop_back();
    // The texture may have been deleted between deferral and now (a level can be
    // evicted while its mips are still pending), so check before touching it.
    if (!glIsTexture(tex)) {
      continue;
    }
    glBindTexture(GL_TEXTURE_2D, tex);
    // FIX 42a: MAX_LEVEL must be restored BEFORE generating. glGenerateMipmap only
    // fills levels up to MAX_LEVEL, so generating with MAX_LEVEL still 0 produced no
    // levels at all, and raising it afterwards left the texture mipmap-INCOMPLETE --
    // every draw using a mipmap min-filter then sampled black.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1000);
    glGenerateMipmap(GL_TEXTURE_2D);
    // FIX 42a self-check, once per run: if level 1 has no storage the texture is
    // mipmap-INCOMPLETE and every mipmap-filtered draw using it renders black. That is
    // exactly what the first FIX 42 build shipped, so it is worth one line to prove the
    // chain is really being built.
    static bool s_checked = false;
    if (!s_checked) {
      s_checked = true;
      GLint w0 = 0, w1 = 0;
      glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w0);
      glGetTexLevelParameteriv(GL_TEXTURE_2D, 1, GL_TEXTURE_WIDTH, &w1);
      fmt::print("[loader] mipmap self-check: level0 w={} level1 w={} -> {}\n", w0, w1,
                 (w0 <= 1 || w1 > 0) ? "COMPLETE" : "INCOMPLETE (textures will be black!)");
    }
    done++;
  }
  return done;
}

// The stage code below keeps using the old constant names; they now read the
// adaptive budget (FIX 36). Desktop never retunes it, so behavior is unchanged.
#define MAX_STAGE_UPLOAD_KB g_loader_budget.stage_kb
#define MAX_TEX_BYTES_PER_FRAME g_loader_budget.tex_bytes
#define LOAD_BUDGET g_loader_budget.ms

// ---------------------------------------------------------------------------
// FIX 33 (AI-assisted): band size for chunked texture uploads. Uploading a
// whole texture (glTexImage2D with data + mipmap generation) is atomic and
// big textures (256x256+) stalled frames for 5-45 ms each on the Switch.
// Storage is allocated with a null upload first, then pixel rows stream in
// via glTexSubImage2D bands so each frame stays inside the load budget. The
// mipmap chain is generated once, after the base level is complete.
// ---------------------------------------------------------------------------
#ifdef __SWITCH__
constexpr int TEX_BAND_BYTES = 128 * 1024;
#else
constexpr int TEX_BAND_BYTES = 512 * 1024;
#endif

#ifdef __SWITCH__
// FIX 34 (AI-assisted): per-texture upload/mipgen instrumentation, so the split
// between "glTexImage2D cost" and "glGenerateMipmap cost" is visible in
// gk_stdout.txt on the console. If mipgen dominates after the GL_UNSIGNED_BYTE
// swap, the next step is CPU box-filtered mipmaps on the loader thread.
static double g_tex_upload_ms = 0.0;
static int g_tex_uploaded = 0;
#endif

namespace {
// FIX 39 (AI-assisted): GL_MAX_TEXTURE_MAX_ANISOTROPY is a driver constant, yet it was
// re-queried with glGetFloatv for every uploaded texture (848 of them for one city
// level, and every streamed texture during gameplay). glGet is a synchronous round-trip
// into the driver - on Tegra exactly the sort of call that serialises against in-flight
// GPU work, and the texture stage measures ~2.2 ms per texture. Query it once.
float cached_max_anisotropy() {
  static float s_aniso = -1.f;
  if (s_aniso < 0.f) {
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &s_aniso);
  }
  return s_aniso;
}
}  // namespace

/*!
 * Upload a texture to the GPU, and give it to the pool.
 */
// FIX 48 (AI-assisted): ASK NOUVEAU FOR A FORMAT IT CAN COPY DIRECTLY.
//
// FIX 46 fixed the level cap churn, and the FIX 46 hardware log proves it worked
// (jak2: `live 7/9, hold 4`, i.e. live above hold, and every level reaching
// "ready in"). But the loads still take 4-6 s, and the FIX 46 log says exactly
// why -- the texture upload dominates, and it gets *worse as the run goes on*:
//
//   [loader] tex stage: 1222 textures, upload 2132.1ms    <- 2.1 s for ONE level
//   TOTAL upload 7681 ms over 15 stages (jak2, one session)
//   per texture:  0.76 ms/texture at boot  ->  2.43 ms/texture later   (3.2x)
//
// FIX 47 read this as "nouveau is byte-swapping every pixel CPU-side and we must
// pre-swap to spare it", and implemented a swap. The swap was wrong (purple
// textures on hardware) and, more importantly, unnecessary: the premise that a
// swap is required was itself mistaken.
//
// What is actually going on: glTexImage2D performs a format/type conversion on
// the calling thread before handing pixels to the driver. Asking for
// GL_UNSIGNED_BYTE with GL_RGBA when the data is already tightly packed RGBA
// bytes gives the implementation a straight memcpy -- no per-pixel conversion at
// all. That is the entire optimisation, and it needs no transformation of our
// data, because our data is already in exactly that layout.
//
// This is deliberately narrow: it changes only the *format handed to GL*, not
// where uploads happen or how they are budgeted, so the FIX 39/42/46 budget
// machinery still governs the frame.
//
// The testable claim is that this is byte-for-byte equivalent to the
// GL_UNSIGNED_INT_8_8_8_8_REV path used on desktop. If it is not, colours will
// be wrong again -- so check the first boot screenshot before benchmarking.
namespace {
// FIX 47 (AI-assisted): retained only so the call sites and the header's
// Switch/desktop split do not have to change. FIX 48 made the copy unnecessary:
// the data already is in the layout GL_UNSIGNED_BYTE wants, so there is nothing
// to stage and nothing to release.
std::mutex& swapped_mutex() {
  static std::mutex s_mutex;
  return s_mutex;
}
}  // namespace

#ifdef __SWITCH__
/*!
 * FIX 48: NO-OP, and deliberately so.
 *
 * FIX 47 staged a byte-swapped copy here to avoid nouveau's per-pixel swizzle in
 * glTexImage2D. Two things turned out to be true:
 *
 *  1. The swap is not needed at all. tfrag3::Texture::data holds 0xAABBGGRR words
 *     (see texture_conversion.h:219, `(a << 24) | (b << 16) | (g << 8) | r`).
 *     GL_UNSIGNED_INT_8_8_8_8_REV lists its components MSB->LSB as A,B,G,R, which
 *     is that same order; GL_UNSIGNED_BYTE + GL_RGBA reads successive bytes as
 *     R,G,B,A, which on a little-endian host is the LSB-first reading of the very
 *     same word. The two formats already describe identical pixels, so the old
 *     path and the new path agree with no transformation whatsoever.
 *
 *  2. Because there is no transformation, there is also no host-side per-pixel
 *     work to move off the render thread. The real cost FIX 47 set out to remove
 *     was nouveau's internal conversion, and that is removed simply by asking for
 *     GL_UNSIGNED_BYTE -- the upload is then a straight copy regardless.
 *
 * Keep the function so the loader-thread call site stays intact; it costs a
 * mutex-free early-out per texture.
 */
void prime_texture_swap(const tfrag3::Texture&) {
  // Intentionally empty -- see above. Nothing to stage.
}

/*!
 * FIX 48: NO-OP, kept for the same reason as prime_texture_swap().
 */
void release_texture_swap(const tfrag3::Texture&) {
  // Intentionally empty -- nothing is ever staged.
}

/*!
 * FIX 48: always zero; no swaps are ever outstanding.
 */
size_t texture_swap_pending() {
  return 0;
}
#endif

// ---------------------------------------------------------------------------
// FIX 69 (AI-assisted): PBO async texture upload (Switch only).
//
// The F67 hardware logs put the texture submit at ~1.6 ms per texture
// (g_tex_upload_ms; the "slow setup" lines showed 6-8.5 ms of it per frame
// while a level streams) -- all of it inside glTexImage2D copying from
// *client* memory: the render thread synchronously feeding texture bytes to
// the driver. FIX 68 spends that time more generously but cannot remove it;
// the loader budget has nothing left to give. FIX 69 instead stages the bytes
// in a GL_PIXEL_UNPACK_BUFFER first and issues the same glTexImage2D from
// buffer offset 0, so the driver can hand the storage to the GPU without the
// render thread waiting on the copy. If nouveau takes the async path, the
// per-texture submit drops well under 1 ms and the stream-in dip shrinks by
// most of the loader submit line; if it silently falls back to a synchronous
// copy, g_tex_upload_ms says so in one hardware run and we stop here.
//
// This is NOT the FIX 33 banding: that crash was glTexSubImage2D from client
// memory in row bands interacting with nouveau's partial-upload staging
// (esr=0x92000007, one 128 KB band off an unmapped page). FIX 69 uploads
// whole textures from a buffer object -- the stock GL 2.0 path every driver
// has. Even so, after FIX 33a no untested texture path ships on this driver
// without a way back:
//
// Kill-switch (same pattern as sdmc:/gk_no_vag.txt, iso.cpp): create
// sdmc:/gk_nopbo.txt on the card and every texture takes the atomic
// glTexImage2D path again -- pure FIX 68 behaviour, same build, so the two
// paths can be A/B-ed on hardware by moving one file. The choice is made
// once per process, at the first texture, and logged either way.
//
// ---------------------------------------------------------------------------
// HARDWARE VERDICT (2026-10-01, jak2, first F69 session): **REJECTED.**
// The card test showed the exact failure mode the kill-switch was built for,
// inverted: instead of taking the async path, nouveau turned every
// glBufferData-refill of the still-in-use PBO into an implicit sync -- the
// submit waited for the GPU to drain the previous upload. Measured: `slow
// setup` texture-stage lines up to **56.5 ms** (F67/F68 baseline 6-8.5 ms),
// 1013 occurrences, loads visibly slower and the fps dip deeper -- exactly
// what the player reported. The path is now DISABLED BY DEFAULT and strictly
// opt-in: create sdmc:/gk_pbo.txt to re-enable it (next experiment would be
// per-texture dedicated PBOs or a fence/defer pattern, not this one).
// The atomic glTexImage2D path stays the only shipped texture path on
// nouveau (FIX 33a's rule stands).
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
#ifdef __SWITCH__
namespace {
bool fix69_pbo_enabled() {
  // Verdict above: OFF unless sdmc:/gk_pbo.txt opts in. (gk_nopbo.txt, from
  // the A/B session, is honored too and forces off even when gk_pbo.txt is
  // present -- explicit off beats explicit on.)
  if (access("sdmc:/gk_nopbo.txt", F_OK) == 0) {
    fmt::print("[loader] FIX 69 PBO upload DISABLED (sdmc:/gk_nopbo.txt present)\n");
    return false;
  }
  if (access("sdmc:/gk_pbo.txt", F_OK) == 0) {
    fmt::print("[loader] FIX 69 PBO upload enabled (opt-in via sdmc:/gk_pbo.txt)\n");
    return true;
  }
  fmt::print("[loader] FIX 69 PBO upload disabled by default (hardware-rejected)\n");
  return false;
}

/*!
 * Upload tex.w * tex.h pixels from a persistent PBO. Returns false when the
 * texture violates the size invariant (check_tex_invariant's warning came too
 * late to be safe here): a short PBO would make glTexImage2D read past the end
 * of the buffer object, which is exactly the class of fault FIX 33a taught us
 * not to gamble on. The caller falls back to the atomic path in that case.
 */
bool fix69_pbo_upload(const tfrag3::Texture& tex) {
  if ((u64)tex.w * tex.h != tex.data.size()) {
    return false;
  }
  static GLuint pbo = 0;
  if (pbo == 0) {
    glGenBuffers(1, &pbo);
  }
  const GLsizeiptr bytes = (GLsizeiptr)(tex.data.size() * 4);
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);
  // Orphan + refill: glBufferData returns fresh storage, so a GPU still
  // draining the previous upload cannot race this memcpy.
  glBufferData(GL_PIXEL_UNPACK_BUFFER, bytes, tex.data.data(), GL_STREAM_DRAW);
  // The same call as the atomic path -- but the "pointer" is offset 0 into
  // the bound PBO, not client memory.
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tex.w, tex.h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
               (const GLvoid*)0);
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
  return true;
}
}  // namespace
#endif

// ---------------------------------------------------------------------------
// FIX 88 (AI-assisted): PBO RING staging for the BCn mip chain (Switch only).
//
// The f87 hardware log measured the accepted-storage BCn path (FIX 74c) at
// 4.5-12.7 ms PER TEXTURE:
//
//   [loader] tex stage: 1222 textures, upload 9762.9ms   <-  8.0 ms/tex
//   [loader] tex stage:  738 textures, upload 8844.1ms   <- 12.0 ms/tex
//
// That is 4-10x the RGBA path it replaced (1.2-1.4 ms/tex, f73c) despite
// moving 4-8x fewer bytes. The cost is the per-mip glCompressedTexSubImage2D
// loop itself: every call hands the driver a CLIENT pointer, and mesa/nouveau
// answers each one with validation + a transient staging allocation + a
// synchronous copy - roughly a millisecond of render-thread time, 5-11 times
// per texture (once per mip level). ~18 s of that in a single city session is
// the "feels like before the compression" the user reported: BCn shrank the
// bytes, but the CALLS are the cost.
//
// FIX 69 already tried PBO staging for the RGBA path and was hardware
// REJECTED (2026-10-01): it used ONE PBO, orphaned on EVERY texture, so
// nouveau turned each glBufferData-refill into an implicit sync on the
// previous, still-in-flight upload (slow-setup lines up to 56.5 ms). The
// failure mode was the reuse interval, not the PBO itself. FIX 88 stages into
// a ring of 16 slots: a slot is only reused 16 textures later, by which time
// the GPU has drained the frame that read it (uploads submitted 2+ frames ago
// are done under vsync). Even in the worst case - a blackout burst that wraps
// the whole ring inside one frame - the orphan path allocates fresh storage
// instead of corrupting, and a hitch during a black screen is invisible.
//
// With the whole mip chain in one buffer, the per-mip SubImage calls become
// bo-relative (offsets into the bound GL_PIXEL_UNPACK_BUFFER): no client
// pointer, no per-call staging allocation, no synchronous copy. Expected cost
// per texture: one glBufferData (bytes already in cache) + N cheap
// buffer-relative SubImage calls. The tex stage line reports the result
// directly (upload ms / textures), so one hardware run settles it.
//
// VERDICT (f88 hardware run, 2026-10-09): REJECTED. The ring was provably
// active ("FIX 88 BCn PBO ring staging active" in gk_stdout) and the tex
// stage stayed at 8.8-13.1 ms/texture (1222 tex 10.8 s, 738 tex 9.7 s) -
// slightly WORSE than f87's client-pointer 8.0-12.0. Buffer-relative uploads
// cost exactly the same as client-pointer ones, so the ~1 ms/mip is NOT
// staging: it is mesa/nouveau doing expensive per-block work for S3TC after
// the source is fetched. See FIX 89 for the follow-up. Default OFF now;
// sdmc:/gk_pbo.txt opts back in (sdmc:/gk_nopbo.txt stays a hard off).
// ---------------------------------------------------------------------------
#ifdef __SWITCH__
namespace {
constexpr int kPboRingSlots = 16;
constexpr size_t kPboMaxBytes = 512 * 1024;

bool fix88_bcn_pbo_enabled() {
  if (access("sdmc:/gk_nopbo.txt", F_OK) == 0) {
    return false;
  }
  if (access("sdmc:/gk_pbo.txt", F_OK) != 0) {
    return false;
  }
  fmt::print("[texfmt] FIX 88 BCn PBO ring staging enabled via gk_pbo.txt ({} slots, max {} KB)\n",
             kPboRingSlots, (int)(kPboMaxBytes / 1024));
  return true;
}

/*!
 * Orphan + refill the next ring slot with `bytes` from `data` and leave it
 * bound on GL_PIXEL_UNPACK_BUFFER. Returns the slot id, or 0 when the texture
 * is too big for the ring (the caller falls back to the client-pointer loop;
 * textures over 384 KB are already deferred during prefetch by FIX 76e, and
 * none in the seed set exceed 512 KB).
 */
GLuint fix88_stage_pbo(const u8* data, size_t bytes) {
  static GLuint s_ring[kPboRingSlots] = {};
  static int s_next = 0;
  if (bytes == 0 || bytes > kPboMaxBytes) {
    return 0;
  }
  GLuint& slot = s_ring[s_next];
  if (slot == 0) {
    glGenBuffers(1, &slot);
  }
  s_next = (s_next + 1) % kPboRingSlots;
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, slot);
  // Orphan + refill - the standard streaming primitive. Because this slot is
  // not touched again for another kPboRingSlots textures, nouveau can hand
  // back the same storage once drained (the usual case) or allocate fresh
  // storage (the burst case). Neither path waits on the GPU - that wait is
  // what sank the single-PBO FIX 69 build.
  glBufferData(GL_PIXEL_UNPACK_BUFFER, (GLsizeiptr)bytes, data, GL_STREAM_DRAW);
  return slot;
}
}  // namespace
#endif

// ---------------------------------------------------------------------------
// FIX 74 (AI-assisted): S3TC format codes. Our glad was generated without the
// EXT_texture_compression_s3tc extension enums, but the numbers are ABI
// constants - and the formats themselves are universally supported on the
// Switch's Tegra X1 (S3TC is a hardware fixed function there) and on desktop GL.
// ---------------------------------------------------------------------------
#ifndef GL_COMPRESSED_RGB_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGB_S3TC_DXT1_EXT 0x83F0
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif

// ---------------------------------------------------------------------------
// FIX 89 (AI-assisted): S3TC STORAGE PROBE + CONDITIONAL DECODE-TO-RGBA
// (Switch only).
//
// The f88 hardware run rejected the FIX 88 PBO ring (see the verdict note
// above): bo-relative uploads cost the same as client-pointer ones, so the
// 8-13 ms/texture lives inside mesa/nouveau's S3TC handling, past the source
// fetch. The remaining question is WHERE that work leaves the data: if the
// driver DECOMPRESSES S3TC to an uncompressed surface at upload (Tegra's BC
// support is a licensing minefield), then VRAM today already holds
// uncompressed data - we have been paying a decode tax in the world's worst
// place (render thread, per mip, per texture) and getting nothing for it,
// while the f73c RGBA path moved 4-8x MORE bytes in 1.2-1.4 ms/texture.
//
// So FIX 89 ASKS THE DRIVER, once, before the first real upload, using a
// throwaway 4x4 DXT1 texture: GL_TEXTURE_COMPRESSED / GL_TEXTURE_INTERNAL_
// FORMAT / GL_TEXTURE_COMPRESSED_IMAGE_SIZE. A driver that keeps the texture
// compressed answers GL_TRUE + 0x83F0/0x83F1 + 8 bytes; one that decompressed
// answers GL_FALSE, an uncompressed internal format, and the IMAGE_SIZE query
// raises INVALID_OPERATION (swallowed - expected).
//
// - driver keeps S3TC compressed: nothing changes. VRAM savings are real,
//   the upload cost is nouveau's compressed tiling path (unfixable from
//   here, but now measured and named).
// - driver decompresses: every following texture decodes BC1/BC3 to RGBA on
//   the CPU (a few hundred us) and uploads through the fast uncompressed
//   path (f73c class, 6-10x faster) with the SAME VRAM footprint the driver
//   was already allocating. bcn_data stays compressed on disk and in RAM -
//   only the GL upload changes, and the dispatch byte accounting counts
//   w*h*4 again (the per-texture work is RGBA-scale again).
//
// Kill switch: sdmc:/gk_nodecode.txt forces mode 0 even if the probe says
// decompressed (the probe still prints its answer either way).
// ---------------------------------------------------------------------------
#ifdef __SWITCH__
namespace {
#ifndef GL_TEXTURE_COMPRESSED_IMAGE_SIZE
#define GL_TEXTURE_COMPRESSED_IMAGE_SIZE 0x86A0
#endif
#ifndef GL_TEXTURE_COMPRESSED
#define GL_TEXTURE_COMPRESSED 0x86A1
#endif
#ifndef GL_TEXTURE_INTERNAL_FORMAT
#define GL_TEXTURE_INTERNAL_FORMAT 0x1003
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#endif

// -1 = not probed yet, 0 = keep glCompressedTex* uploads, 1 = decode + RGBA.
int s_fix89_mode = -1;

bool fix89_decode_active() {
  return s_fix89_mode == 1;
}

constexpr u32 fix89_565(u16 c, u32 a) {
  const u32 r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
  return (a << 24) | (((b << 3) | (b >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) |
         ((r << 3) | (r >> 2));
}

// channel-wise (2*a + b) / 3 style blend of two packed RGBA8 colors; alpha from `a_out`
u32 fix89_mix(u32 c0, u32 c1, u32 w0, u32 w1, u32 div, u32 a_out) {
  u32 out = a_out << 24;
  for (int sh = 0; sh < 24; sh += 8) {
    const u32 v = (((c0 >> sh) & 0xff) * w0 + ((c1 >> sh) & 0xff) * w1) / div;
    out |= v << sh;
  }
  return out;
}

/*!
 * Decode one BC1 (opaque DXT1, matching GL_COMPRESSED_RGB_S3TC_DXT1_EXT) or BC3
 * mip into tightly packed RGBA8 `dst` (w*h pixels). Edge blocks of non-multiple-
 * of-4 / sub-4x4 mips are clipped. Caller validated the source length.
 */
void fix89_decode_bcn_mip(const u8* src, u32 w, u32 h, bool is_bc1, u32* dst) {
  const u32 bw = (w + 3) / 4, bh = (h + 3) / 4;
  for (u32 by = 0; by < bh; by++) {
    for (u32 bx = 0; bx < bw; bx++) {
      u8 alpha[16];
      if (!is_bc1) {
        const u8 a0 = src[0], a1 = src[1];
        u8 pal[8] = {a0, a1};
        if (a0 > a1) {
          for (int i = 1; i < 7; i++) {
            pal[i + 1] = u8(((7 - i) * a0 + i * a1) / 7);
          }
        } else {
          for (int i = 1; i < 5; i++) {
            pal[i + 1] = u8(((5 - i) * a0 + i * a1) / 5);
          }
          pal[6] = 0;
          pal[7] = 255;
        }
        u64 bits = 0;
        for (int i = 0; i < 6; i++) {
          bits |= u64(src[2 + i]) << (8 * i);
        }
        for (int i = 0; i < 16; i++) {
          alpha[i] = pal[(bits >> (3 * i)) & 7];
        }
        src += 8;
      }
      const u16 c0 = u16(src[0] | (src[1] << 8));
      const u16 c1 = u16(src[2] | (src[3] << 8));
      const u32 idx = u32(src[4]) | (u32(src[5]) << 8) | (u32(src[6]) << 16) | (u32(src[7]) << 24);
      src += 8;
      u32 pal[4];
      pal[0] = fix89_565(c0, 255);
      pal[1] = fix89_565(c1, 255);
      // BC3 color blocks always use the 4-color mode; BC1 picks by c0 > c1.
      if (!is_bc1 || c0 > c1) {
        pal[2] = fix89_mix(pal[0], pal[1], 2, 1, 3, 255);
        pal[3] = fix89_mix(pal[0], pal[1], 1, 2, 3, 255);
      } else {
        pal[2] = fix89_mix(pal[0], pal[1], 1, 1, 2, 255);
        pal[3] = 0xff000000;  // RGB DXT1: "transparent" index decodes to opaque black
      }
      for (u32 py = 0; py < 4; py++) {
        const u32 y = by * 4 + py;
        if (y >= h) {
          break;
        }
        for (u32 px = 0; px < 4; px++) {
          const u32 x = bx * 4 + px;
          if (x >= w) {
            break;
          }
          const u32 i = py * 4 + px;
          u32 c = pal[(idx >> (2 * i)) & 3];
          if (!is_bc1) {
            c = (c & 0x00ffffff) | (u32(alpha[i]) << 24);
          }
          dst[y * w + x] = c;
        }
      }
    }
  }
}

/*!
 * Run once, on the first BCn texture. Uploads a throwaway 4x4 DXT1 texture and
 * asks the driver how it stored it, then picks s_fix89_mode. Leaves texture
 * unit 0 bound to 0 - the caller rebinds its own texture.
 */
void fix89_probe_s3tc_storage() {
  while (glGetError() != GL_NO_ERROR) {
  }
  GLuint probe = 0;
  glGenTextures(1, &probe);
  glBindTexture(GL_TEXTURE_2D, probe);
  const u8 block[8] = {0x00, 0xf8, 0x1f, 0x00, 0xe4, 0xe4, 0xe4, 0xe4};  // red/blue gradient
  glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGB_S3TC_DXT1_EXT, 4, 4, 0, 8, block);
  const GLenum upload_err = glGetError();

  GLint is_compressed = -1, internal_fmt = -1, image_size = -1;
  if (glad_glGetTexLevelParameteriv) {
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_COMPRESSED, &is_compressed);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &internal_fmt);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_COMPRESSED_IMAGE_SIZE, &image_size);
  }
  while (glGetError() != GL_NO_ERROR) {
  }  // IMAGE_SIZE is not an ES 3.1 pname - INVALID_ENUM there is expected
  glBindTexture(GL_TEXTURE_2D, 0);
  glDeleteTextures(1, &probe);

  // Decompressed = the driver said "not compressed" or reported a non-S3TC format.
  const bool known = is_compressed != -1;
  const bool driver_decompresses =
      known && (is_compressed == GL_FALSE ||
                (internal_fmt != GL_COMPRESSED_RGB_S3TC_DXT1_EXT &&
                 internal_fmt != GL_COMPRESSED_RGBA_S3TC_DXT1_EXT));
  const bool force_off = access("sdmc:/gk_nodecode.txt", F_OK) == 0;
  // mesa answers these queries from the LOGICAL format, so a driver-side
  // fallback can still say "compressed"; gk_decode.txt forces the decode path
  // for a hardware A/B regardless of the probe.
  const bool force_on = access("sdmc:/gk_decode.txt", F_OK) == 0;
  // FIX 90 (AI-assisted): decode is now the DEFAULT. The f89 A/B proved the
  // per-texture cost is the number of upload calls (one per mip), not the
  // bytes or the format, so decode + level-0-only upload is the fast path
  // whatever the probe says. gk_nodecode.txt restores the f88 BCn path.
  (void)driver_decompresses;
  s_fix89_mode = force_off ? 0 : 1;

  fmt::print(
      "[texfmt] FIX 89 S3TC probe: upload_err=0x{:x} compressed={} internal=0x{:x} size={} -> "
      "driver {}; mode={} ({}){}\n",
      upload_err, is_compressed, internal_fmt, image_size,
      !known ? "UNKNOWN" : (driver_decompresses ? "DECOMPRESSES" : "keeps S3TC"), s_fix89_mode,
      s_fix89_mode ? "CPU decode + RGBA8 upload" : "glCompressedTex* upload",
      force_off ? " [gk_nodecode.txt]" : (force_on ? " [gk_decode.txt]" : ""));
}
}  // namespace

void decode_level_bcn_to_rgba(tfrag3::Level& level) {
  static const bool s_disabled = access("sdmc:/gk_nodecode.txt", F_OK) == 0;
  if (s_disabled) {
    return;
  }
  Timer t;
  int n = 0;
  for (auto& tex : level.textures) {
    if (tex.format == tfrag3::TEXTURE_FMT_RGBA || tex.w == 0 || tex.h == 0 ||
        tex.mip_offsets.empty()) {
      continue;
    }
    const bool is_bc1 = tex.format == tfrag3::TEXTURE_FMT_BC1;
    const u64 blocks = u64((tex.w + 3) / 4) * ((tex.h + 3) / 4);
    if (u64(tex.mip_offsets[0]) + blocks * (is_bc1 ? 8 : 16) > tex.bcn_data.size()) {
      continue;  // malformed: leave it for upload_bcn_texture's 1x1-black handling
    }
    tex.data.resize(size_t(tex.w) * tex.h);
    fix89_decode_bcn_mip(tex.bcn_data.data() + tex.mip_offsets[0], tex.w, tex.h, is_bc1,
                         tex.data.data());
    tex.format = tfrag3::TEXTURE_FMT_RGBA;
    std::vector<u8>().swap(tex.bcn_data);
    std::vector<u32>().swap(tex.mip_offsets);
    n++;
  }
  if (n) {
    fmt::print("[texfmt] FIX 91 loader-thread decode: {} BCn textures -> RGBA in {:.1f}ms\n", n,
               t.getMs());
  }
}
#else
namespace {
constexpr bool fix89_decode_active() {
  return false;
}
}  // namespace
#endif

namespace {
// FIX 74: upload one pre-compressed, pre-mipped texture. Every mip level comes
// straight from the fr3 via glCompressedTexImage2D, so there is no
// glGenerateMipmap at all (the ~1s/level deferred-mipgen backlog is gone) and
// the bytes moved are 4x (BC1) to 8x (BC3) smaller than the RGBA path.
//
// FIX 33a's rule: nothing unvalidated goes near nouveau's upload path. The
// mip offset table is checked against bcn_data.size() with the exact size the
// driver expects; a malformed file logs loudly, gets a 1x1 black texture, and
// the game keeps running.
bool upload_bcn_texture(GLuint gl_tex, const tfrag3::Texture& tex) {
  static bool s_bcn_reported = false;
  if (!s_bcn_reported) {
    s_bcn_reported = true;
    fmt::print("[texfmt] FIX 74 BCn compressed texture path active (first texture format {})\n",
               tex.format);
  }
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, gl_tex);

  const bool is_bc1 = tex.format == tfrag3::TEXTURE_FMT_BC1;
  const GLenum gl_format =
      is_bc1 ? GL_COMPRESSED_RGB_S3TC_DXT1_EXT : GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
  const u32 bytes_per_block = is_bc1 ? 8 : 16;
  const size_t n_mips = tex.mip_offsets.size();

  // validate every level before uploading any of it
  bool valid = tex.w > 0 && tex.h > 0 && n_mips >= 1 && n_mips <= 16;
  for (size_t m = 0; valid && m < n_mips; m++) {
    const u32 mw = std::max(1u, (u32)tex.w >> m);
    const u32 mh = std::max(1u, (u32)tex.h >> m);
    const u64 mip_len = u64(((mw + 3) / 4) * ((mh + 3) / 4)) * bytes_per_block;
    valid = u64(tex.mip_offsets[m]) + mip_len <= u64(tex.bcn_data.size());
  }
  if (!valid) {
    fmt::print("[loader] FIX 74 BAD BCn texture {}x{} fmt {} ({} mips, {} bytes) - 1x1 black\n",
               tex.w, tex.h, tex.format, n_mips, tex.bcn_data.size());
    const u32 black = 0xff000000;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &black);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    return false;
  }

  // FIX 74c (AI-assisted): glTexStorage2D only accepts levels <=
  // floor(log2(max(w,h))) + 1. A full chain down to 1x1 satisfies this exactly,
  // but a malformed over-deep offset table would not - clamp so the storage
  // call below is always legal (entries past 1x1 carry no real image anyway).
  int legal_levels = 1;
  for (u32 s = std::max(tex.w, tex.h); (s >>= 1) != 0;) {
    legal_levels++;
  }
  const size_t n_use = std::min(n_mips, size_t(legal_levels));

#ifdef __SWITCH__
  // FIX 89 (AI-assisted): ask the driver once whether S3TC actually stays
  // compressed in VRAM. If it decompresses, decode to RGBA here and take the
  // fast uncompressed upload instead - same VRAM as the driver was already
  // allocating, a fraction of the render-thread time. (The probe binds its
  // own texture; rebind ours before any path below touches state.)
  if (s_fix89_mode < 0) {
    fix89_probe_s3tc_storage();
    glBindTexture(GL_TEXTURE_2D, gl_tex);
  }
  if (s_fix89_mode == 1) {
    // FIX 90 (AI-assisted): ONE upload call per texture. The f89 session with
    // per-mip RGBA uploads cost exactly what the per-mip BCn path did (1222 tex
    // in 10.2 s vs 10.0 s) - nouveau charges ~1 ms per glTex*Image call
    // regardless of size or format. So decode level 0 only, upload it with the
    // same atomic glTexImage2D the f73c RGBA path used (1.2 ms/tex), and let
    // the FIX 42 mip queue build the chain on the GPU later.
    static std::vector<u32> s_scratch;  // loader thread only
    const size_t need = size_t(tex.w) * tex.h;
    if (s_scratch.size() < need) {
      s_scratch.resize(need);
    }
    fix89_decode_bcn_mip(tex.bcn_data.data() + tex.mip_offsets[0], tex.w, tex.h, is_bc1,
                         s_scratch.data());
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tex.w, tex.h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 s_scratch.data());
#if GOAL_DEFER_MIPMAPS
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    if (n_use > 1) {
      mipq_defer(gl_tex);
    }
#else
    glGenerateMipmap(GL_TEXTURE_2D);
#endif
    static bool s_fix90_reported = false;
    if (!s_fix90_reported) {
      s_fix90_reported = true;
      fmt::print("[texfmt] FIX 90 decode path: level 0 only + deferred mipgen\n");
    }
    return true;
  }
#endif

  // FIX 74c (AI-assisted): immutable storage + per-mip sub-image. The f74b
  // hardware logs showed the per-mip glCompressedTexImage2D loop costing
  // ~9.6 ms of driver CPU PER TEXTURE (670-texture levels: 5.7-8.6 s of
  // upload time) - 8x the f73c RGBA path's 1.2-1.4 ms/texture, even though
  // the bytes moved are 4-8x smaller. Each TexImage call makes mesa/nouveau
  // re-derive/reallocate the texture's storage, and ~11 tiny calls lose to
  // that fixed cost. One glTexStorage2D allocates the whole miptree once;
  // glCompressedTexSubImage2D then just copies blocks into it.
  // Probed once on the first texture (error state is cleared first so a
  // stale error can't poison the probe); if the driver rejects it we fall
  // back to the old loop for the whole session. Later textures can't fail
  // the probe's conditions: dims are validated above and levels are clamped
  // to the legal maximum.
  static int s_texstorage = -1;  // -1 = not probed yet, 1 = use it, 0 = rejected
  bool have_storage = false;
  if (s_texstorage != 0 && glad_glTexStorage2D != NULL) {
    if (s_texstorage < 0) {
      while (glGetError() != GL_NO_ERROR) {
      }
    }
    glTexStorage2D(GL_TEXTURE_2D, GLsizei(n_use), gl_format, tex.w, tex.h);
    if (s_texstorage < 0) {
      s_texstorage = (glGetError() == GL_NO_ERROR) ? 1 : 0;
      fmt::print("[texfmt] FIX 74c glTexStorage2D {} (fmt {})\n",
                 s_texstorage ? "accepted - immutable BCn upload active"
                              : "rejected - per-mip glCompressedTexImage2D fallback",
                 tex.format);
    }
    have_storage = (s_texstorage == 1);
  }

#ifdef __SWITCH__
  // FIX 88 (AI-assisted): stage the whole chain into a ring PBO once, then run
  // the per-mip calls below from buffer offsets instead of client pointers.
  // See the FIX 88 block above for the f87 measurement this removes (4.5-12.7
  // ms of client-copy driver time per texture) and why a ring does not repeat
  // the FIX 69 implicit-sync rejection. `use_pbo` keeps the PBO bound for the
  // loop and the unbind below; the client-pointer loop remains as fallback
  // (oversized texture, or gk_nopbo.txt).
  static const bool s_bcn_pbo = fix88_bcn_pbo_enabled();
  const bool use_pbo =
      s_bcn_pbo && fix88_stage_pbo(tex.bcn_data.data(), tex.bcn_data.size()) != 0;
#endif

  for (size_t m = 0; m < n_use; m++) {
    const u32 mw = std::max(1u, (u32)tex.w >> m);
    const u32 mh = std::max(1u, (u32)tex.h >> m);
    const u32 mip_len = ((mw + 3) / 4) * ((mh + 3) / 4) * bytes_per_block;
    const u8* src = tex.bcn_data.data() + tex.mip_offsets[m];
#ifdef __SWITCH__
    // FIX 88: with the ring PBO bound, the "pointer" is an offset into it -
    // the same bytes, but the driver copies bo-to-bo (or references the bo
    // directly) instead of staging a client pointer per mip.
    if (use_pbo) {
      src = (const u8*)(uintptr_t)(size_t)tex.mip_offsets[m];
    }
#endif
    if (have_storage) {
      glCompressedTexSubImage2D(GL_TEXTURE_2D, GLint(m), 0, 0, mw, mh, gl_format, mip_len, src);
    } else {
      glCompressedTexImage2D(GL_TEXTURE_2D, GLint(m), gl_format, mw, mh, 0, mip_len, src);
    }
  }
#ifdef __SWITCH__
  // FIX 88: never leave GL_PIXEL_UNPACK_BUFFER bound - every other pixel
  // transfer in the process (common textures, the RGBA paths, sky) passes
  // real client pointers and would read from this PBO instead.
  if (use_pbo) {
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
  }
#endif
  // the file's chain is complete down to 1x1 - the texture is mipmap-complete
  // from the first upload, no mipq_defer / glGenerateMipmap round trip needed.
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, GLint(n_use - 1));
  return true;
}
}  // namespace

u64 add_texture(TexturePool& pool, const tfrag3::Texture& tex, bool is_common) {
  GLuint gl_tex;
  glActiveTexture(GL_TEXTURE0);
  glGenTextures(1, &gl_tex);
  glBindTexture(GL_TEXTURE_2D, gl_tex);
#ifdef __SWITCH__
  Timer tex_upload_timer;
  tex_upload_timer.start();
#endif
  // FIX 74 (AI-assisted): pre-compressed (BC1/BC3) texture - all mips upload
  // straight from the fr3, so this skips the FIX 48 RGBA path, the FIX 69 PBO
  // experiment and the FIX 42 mipq entirely. It is the same code on Switch and
  // desktop (desktop exercises it too, so it is not untested Switch-only code).
  if (tex.format != tfrag3::TEXTURE_FMT_RGBA) {
    upload_bcn_texture(gl_tex, tex);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, cached_max_anisotropy());
    if (tex.load_to_pool) {
      TextureInput in;
      in.debug_page_name = tex.debug_tpage_name;
      in.debug_name = tex.debug_name;
      in.w = tex.w;
      in.h = tex.h;
      in.gpu_texture = gl_tex;
      in.common = is_common;
      in.id = PcTextureId::from_combo_id(tex.combo_id);
      // FIX 74: no CPU-side pixels for compressed textures. The only CPU pixel
      // reader is SkyBlendCPU, and the extractor keeps sky textures RGBA, so
      // nullptr here can only reach readers that already null-check.
      in.src_data = nullptr;
      pool.give_texture(in);
    }
#ifdef __SWITCH__
    g_tex_upload_ms += tex_upload_timer.getMs();
    g_tex_uploaded++;
#endif
    g_loader_gpu_submits_this_frame++;
    return gl_tex;
  }
  // FIX 48 (AI-assisted): upload the texture's own storage, unmodified, as
  // GL_UNSIGNED_BYTE.
  //
  // FIX 47 wrapped this in a "primed swap" lookup plus a self-written verifier.
  // The verifier was checking the wrong invariant (it compared against a
  // rotate-by-8 that was itself wrong), so it happily validated a purple image --
  // a good reminder that a check derived from the same misunderstanding as the
  // code it checks proves nothing.
  //
  // There is no transformation to verify. tfrag3::Texture::data is already in the
  // byte order GL_UNSIGNED_BYTE expects; see the note on prime_texture_swap()
  // above for the derivation and the empirical confirmation.
  const void* upload_pixels = (const void*)tex.data.data();
  // FIX 52 (AI-assisted): one count per texture handed to GL, for the GPU-cost probe.
  // Deliberately outside the __SWITCH__ guards: this is the shared upload point, and a
  // frame that submitted work should be measurable wherever it came from. (Which also
  // means desktop builds exercise the same probe, so the logic is not Switch-only code
  // that nobody has ever run.)
  g_loader_gpu_submits_this_frame++;
#ifdef __SWITCH__
  static bool s_swapped_reported = false;
  if (!s_swapped_reported) {
    s_swapped_reported = true;
    fmt::print("[loader] FIX 48 texture path: GL_UNSIGNED_BYTE, no swap (REV-equivalent)\n");
  }
#endif
#ifdef __SWITCH__
  // FIX 69 (AI-assisted): PBO path first (chosen once, at the first texture),
  // atomic glTexImage2D as the fallback -- kill-switch file present, or a
  // texture that failed the size check. s_fix69_pbo's initialiser runs
  // exactly once per process; every later call is a plain bool load.
  static const bool s_fix69_pbo = fix69_pbo_enabled();
  if (!(s_fix69_pbo && fix69_pbo_upload(tex))) {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tex.w, tex.h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 upload_pixels);
  }
  release_texture_swap(tex);
#else
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tex.w, tex.h, 0, GL_RGBA,
               GL_UNSIGNED_INT_8_8_8_8_REV, upload_pixels);
#endif
#ifdef __SWITCH__
  g_tex_upload_ms += tex_upload_timer.getMs();
  g_tex_uploaded++;
#endif
#if GOAL_DEFER_MIPMAPS
  // FIX 42: defer the mip chain out of the load window. MAX_LEVEL 0 keeps the texture
  // mipmap-complete with only level 0, so it renders correctly (just unfiltered in the
  // distance) until mipq_process() gets to it.
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
  mipq_defer(gl_tex);
#else
  glGenerateMipmap(GL_TEXTURE_2D);
#endif
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, cached_max_anisotropy());
  if (tex.load_to_pool) {
    TextureInput in;
    in.debug_page_name = tex.debug_tpage_name;
    in.debug_name = tex.debug_name;
    in.w = tex.w;
    in.h = tex.h;
    in.gpu_texture = gl_tex;
    in.common = is_common;
    in.id = PcTextureId::from_combo_id(tex.combo_id);
    in.src_data = (const u8*)tex.data.data();
    pool.give_texture(in);
  }

  return gl_tex;
}

// ---------------------------------------------------------------------------
// FIX 33a (AI-assisted): the first FIX 33 build crashed on the Switch during
// the boot blackout load: nouveau's glTexSubImage2D client-copy faulted
// reading exactly one 128 KB band (TEX_BAND_BYTES) from an unmapped page
// (gk_fatal esr=0x92000007, memcpy len 0x20000). The identical band code
// boots fine on the host (macOS GL), so the band path interacts badly with
// Mesa/nouveau's partial-upload staging - not worth debugging remotely.
// Decision: on Switch, load textures with the original atomic add_texture()
// path that ran for months (per-frame byte caps keep the frame hitches
// bounded); desktop keeps the banded streaming. The actual FIX 33 crash fix
// (purge-before-load + buffer pooling + eviction) is untouched.
// ---------------------------------------------------------------------------
static void check_tex_invariant(const tfrag3::Texture& tex) {
  // The extractor promises data.size() == w*h (u32s). If a file ever
  // violates that, say so loudly instead of reading out of bounds.
  // FIX 74: compressed textures carry no data[] - their layout is validated
  // per-mip inside upload_bcn_texture instead.
  if (tex.format == tfrag3::TEXTURE_FMT_RGBA && (u64)tex.w * tex.h != tex.data.size()) {
    fmt::print("[loader] TEXTURE SIZE MISMATCH: '{}' ({}x{} = {} px) has {} u32 of data\n",
               tex.debug_name, tex.w, tex.h, (u64)tex.w * tex.h, tex.data.size());
  }
}

class TextureLoaderStage : public LoaderStage {
 public:
  TextureLoaderStage() : LoaderStage("texture") {}
  bool run(Timer& timer, LoaderInput& data) override {
    LevelData& ld = *data.lev_data;
    const auto& all_textures = ld.level->textures;
#ifdef __SWITCH__
    // FIX 33a: original atomic upload path - proven on Tegra/nouveau.
    if (ld.textures.empty() && !all_textures.empty()) {
      // start of a new level: reset the FIX 34 accumulators so boot/common
      // uploads don't pollute this level's numbers
      g_tex_upload_ms = 0.0;
      g_tex_uploaded = 0;
    }
    int bytes_this_run = 0;
    int tex_this_run = 0;
    if (ld.textures.size() < all_textures.size()) {
      std::unique_lock<std::mutex> tpool_lock(data.tex_pool->mutex());
      while (ld.textures.size() < all_textures.size()) {
        const tfrag3::Texture& tex = all_textures[ld.textures.size()];
        check_tex_invariant(tex);
#ifdef __SWITCH__
        // FIX 76e (AI-assisted): BIG-TEXTURE DEFERRAL DURING A PURE PREFETCH.
        // Texture order must be preserved (the level's draw data indexes by
        // position), so a texture we cannot afford this frame defers the
        // whole stage - run() returns not-done and we resume at this exact
        // texture next frame. That is free: the prefetch has minutes of
        // dwell. Dispatching it anyway is not free: one >384KB BCn upload is
        // 3-15 ms of nouveau driver time, and on a vsync-locked frame with
        // ~0 slack that is a guaranteed missed refresh - the f76d "terrible
        // fps in Geyser Rock" was minutes of exactly this. Game loads and
        // blackouts never defer (prefetch_only is false there): a visible
        // stream-in must finish, hitches included (FIX 38). The starvation
        // breaker: if the SAME texture has blocked the queue for ~3 s
        // (~90 frames), dispatch it anyway - one hitch beats a cache that
        // never completes, and the counter resets on the next success.
        if (g_loader_budget.prefetch_only) {
          // FIX 89: with decode-to-RGBA uploads the per-texture work is
          // RGBA scale again, so the >384KB defer check must look at w*h*4.
          const bool fix89_rgba =
              fix89_decode_active() && tex.format != tfrag3::TEXTURE_FMT_RGBA;
          const u32 tex_bytes = (tex.format == tfrag3::TEXTURE_FMT_RGBA || fix89_rgba)
                                    ? (u32)tex.w * tex.h * 4u
                                    : (u32)tex.bcn_data.size();
          if (tex_bytes > 384u * 1024u && m_pf_defer_frames < 90) {
            m_pf_defer_frames++;
            break;
          }
          m_pf_defer_frames = 0;
        }
#endif
        ld.textures.push_back(add_texture(*data.tex_pool, tex, false));
        // FIX 74: account the bytes actually uploaded - compressed textures
        // ship 4-8x fewer bytes (and their mips), so the budget lets the
        // stream finish in proportionally fewer dispatches.
        // FIX 89: decoded textures cost RGBA-scale upload time again, so the
        // dispatch budget must count w*h*4 for them, not the compressed size.
        const bool fix89_rgba_acc =
            fix89_decode_active() && tex.format != tfrag3::TEXTURE_FMT_RGBA;
        bytes_this_run += (tex.format == tfrag3::TEXTURE_FMT_RGBA || fix89_rgba_acc)
                              ? tex.w * tex.h * 4
                              : (int)tex.bcn_data.size();
        tex_this_run++;
        // FIX 46c (AI-assisted): the hard per-dispatch cap.
        //
        // FIX 33a routes Switch textures through the atomic add_texture() path (the
        // banded glTexSubImage2D path crashed nouveau), so the only thing bounding a
        // frame is the number of textures dispatched per update(). That was 20 --
        // and the measured per-texture cost after FIX 39 is ~0.9 ms, so one frame
        // could spend ~18 ms on texture upload alone. On a 33.3 ms target that is
        // more than half a frame, every frame, for the whole duration of a load:
        // the "huge slowdown" while an area streams in. The recorded totals
        // (`tex stage: 606 textures, upload 905.2ms`) are level-wide sums, but at
        // 20/frame they were being paid 18 ms at a time in exactly the frames the
        // player is watching.
        //
        // The cap now follows the same "does this frame have room?" rule as the
        // byte/time budget below, so it stays at 20 when there is headroom (a
        // blackout, or a frame comfortably under target) and drops to 4 when the
        // frame is already missing target -- spreading the same work over more
        // frames instead of deepening a stall.
        //
        // FIX 66 (AI-assisted): the cap now follows the BYTE budget, not a binary
        // ms>=8 test. The old test read only g_loader_budget.ms, but the byte cap
        // is set by an independent signal: catchup-pace (5 ms) and idle-healthy
        // (4 ms) grant a full 1 MB/frame yet were allowed only 4 textures
        // (~0.25-1 MB at 256x256x4 per texture) - so the count, not the bytes,
        // was the binding cap, and the 1 MB grant was unreachable. Walking into a
        // new area with the frame dipping to 25-29 fps lands exactly on
        // catchup-pace, which turned every 400-600 texture section into 3-5+ s of
        // visible pop-in in ALL games (this stage sits after the per-game I/O
        // layers, so jak2/jak3's FIX 45-65 read-ahead could not help it).
        // Scaling the count from tex_bytes keeps FIX 46c's protection - when the
        // frame truly has no room the tiers drop tex_bytes to 128-256 KB and the
        // count drops with it - and it also makes FIX 52's GPU-cost scaling slow
        // the dispatch rate, which the ms-keyed count cap ignored entirely.
        // 128 KB -> 4 (clamped up from 2), 256 KB -> 4, 512 KB -> 8, 1 MB -> 16,
        // 2 MB and up -> 20 (clamped).
        // FIX 94: the upper clamp follows dispatch_cap, so the frozen blackout/sweep
        // tiers (cap 40/64) are not silently held at 20.
        const int max_tex_this_dispatch =
            std::min(std::clamp<int>(g_loader_budget.tex_bytes / (64 * 1024), 4,
                                     std::max<int>(20, (int)g_loader_budget.dispatch_cap)),
                     (int)g_loader_budget.dispatch_cap);
        if (tex_this_run >= max_tex_this_dispatch) {
          break;
        }
        if ((u32)bytes_this_run > MAX_TEX_BYTES_PER_FRAME || timer.getMs() > LOAD_BUDGET) {
          break;
        }
      }
    }
    const bool finished = ld.textures.size() == all_textures.size();
    if (finished && !all_textures.empty() && g_tex_uploaded > 0 && !m_logged_stats) {
      // FIX 34: where did the texture staging time actually go?
      // FIX 42a: mipgen is no longer part of the load window, so report the deferred
      // backlog instead -- that is the number that matters now.
      fmt::print("[loader] tex stage: {} textures, upload {:.1f}ms, {} mip chains deferred\n",
                 g_tex_uploaded, g_tex_upload_ms, mipq_pending());
      m_logged_stats = true;
    }
    return finished;
#else
    while (ld.textures.size() < all_textures.size()) {
      const tfrag3::Texture& tex = all_textures[ld.textures.size()];
      check_tex_invariant(tex);
      if (tex.format != tfrag3::TEXTURE_FMT_RGBA) {
        // FIX 74: compressed textures carry their own mip chain and are 4-8x
        // smaller than RGBA - upload them atomically instead of banding.
        std::unique_lock<std::mutex> tpool_lock(data.tex_pool->mutex());
        ld.textures.push_back(add_texture(*data.tex_pool, tex, false));
        if (timer.getMs() > LOAD_BUDGET) {
          return false;
        }
        continue;
      }
      if (!m_cur_allocated) {
        // allocate storage + params now; pixels stream in via row bands
        // below (FIX 33: no more atomic full-texture glTexImage2D uploads).
        glActiveTexture(GL_TEXTURE0);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glGenTextures(1, &m_cur_tex);
        glBindTexture(GL_TEXTURE_2D, m_cur_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tex.w, tex.h, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, nullptr);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, cached_max_anisotropy());
        m_cur_allocated = true;
        m_cur_row = 0;
      }

      // FIX 33a: never trust w*h more than the data we actually have.
      const int row_bytes = tex.w * 4;
      const int valid_rows =
          row_bytes > 0 ? (int)std::min<u64>((u64)tex.h, tex.data.size() / tex.w) : 0;
      while (m_cur_row < valid_rows) {
        const int rows =
            std::min<int>(valid_rows - m_cur_row, std::max(1, TEX_BAND_BYTES / row_bytes));
        glActiveTexture(GL_TEXTURE0);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glBindTexture(GL_TEXTURE_2D, m_cur_tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, m_cur_row, tex.w, rows, GL_RGBA,
                        GL_UNSIGNED_BYTE,
                        tex.data.data() + (size_t)m_cur_row * row_bytes);
        m_cur_row += rows;
        if (timer.getMs() > LOAD_BUDGET) {
          return false;  // out of budget mid-texture; resume next frame
        }
      }
      if (m_cur_row < tex.h) {
        // invariant was violated: the missing rows stay uninitialized, but
        // we must still finish the texture so the stage can move on.
        m_cur_row = tex.h;
      }

      // base level complete -> build the mipmap chain once.
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, m_cur_tex);
#if GOAL_DEFER_MIPMAPS
      // FIX 42: deferred; see LoaderStages.h.
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
      mipq_defer(m_cur_tex);
#else
      glGenerateMipmap(GL_TEXTURE_2D);
#endif
      if (tex.load_to_pool) {
        TextureInput in;
        in.debug_page_name = tex.debug_tpage_name;
        in.debug_name = tex.debug_name;
        in.w = tex.w;
        in.h = tex.h;
        in.gpu_texture = m_cur_tex;
        in.common = false;
        in.id = PcTextureId::from_combo_id(tex.combo_id);
        in.src_data = (const u8*)tex.data.data();
        std::unique_lock<std::mutex> tpool_lock(data.tex_pool->mutex());
        data.tex_pool->give_texture(in);
      }
      ld.textures.push_back(m_cur_tex);
      m_cur_tex = 0;
      m_cur_allocated = false;
      if (timer.getMs() > LOAD_BUDGET && ld.textures.size() < all_textures.size()) {
        return false;
      }
    }
    return true;
#endif
  }

  void reset() override {
    if (m_cur_allocated && m_cur_tex != 0) {
      // defensive: the stage should never be reset mid-texture, but if it
      // ever happens, don't leak the half-uploaded texture.
      glDeleteTextures(1, &m_cur_tex);
    }
    m_cur_tex = 0;
    m_cur_allocated = false;
    m_cur_row = 0;
    m_logged_stats = false;
#ifdef __SWITCH__
    // FIX 76e: fresh level -> no deferral history.
    m_pf_defer_frames = 0;
#endif
  }

 private:
  GLuint m_cur_tex = 0;
  bool m_cur_allocated = false;
  int m_cur_row = 0;
  bool m_logged_stats = false;
#ifdef __SWITCH__
  // FIX 76e (AI-assisted): consecutive frames the stage has deferred the
  // current oversized texture during a pure prefetch (see run()). Used as a
  // starvation breaker: after ~3 s blocked on the same texture, push it
  // through and accept the one hitch - a cache that never finishes is worse.
  int m_pf_defer_frames = 0;
#endif
};

class TfragLoadStage : public LoaderStage {
 public:
  TfragLoadStage() : LoaderStage("tfrag") {}
  bool run(Timer& timer, LoaderInput& data) override {
    if (m_done) {
      return true;
    }

    if (data.lev_data->level->tfrag_trees.front().empty()) {
      m_done = true;
      return true;
    }

    if (!m_opengl_created) {
      for (int geo = 0; geo < tfrag3::TFRAG_GEOS; geo++) {
        auto& in_trees = data.lev_data->level->tfrag_trees[geo];
        for (auto& in_tree : in_trees) {
          // FIX 33: pooled buffers (see GpuBufferPool.h).
          GLuint& tree_out = data.lev_data->tfrag_vertex_data[geo].emplace_back();
          tree_out = data.buffers->acquire(
              GL_ARRAY_BUFFER,
              (GLsizeiptr)in_tree.unpacked.vertices.size() * sizeof(tfrag3::PreloadedVertex));
        }
      }
      m_opengl_created = true;
      return false;
    }

    constexpr u32 CHUNK_SIZE = STAGE_VERT_CHUNK;
    u32 uploaded_bytes = 0;
    [[maybe_unused]] u32 unique_buffers = 0;

    while (true) {
      bool complete_tree;

      if (data.lev_data->level->tfrag_trees[m_next_geo].empty()) {
        complete_tree = true;
      } else {
        const auto& tree = data.lev_data->level->tfrag_trees[m_next_geo][m_next_tree];
        u32 end_vert_in_tree = tree.unpacked.vertices.size();
        // the number of vertices we'd need to finish the tree right now
        size_t num_verts_left_in_tree = end_vert_in_tree - m_next_vert;
        size_t start_vert_for_chunk;
        size_t end_vert_for_chunk;

        if (num_verts_left_in_tree > CHUNK_SIZE) {
          complete_tree = false;
          // should only do partial
          start_vert_for_chunk = m_next_vert;
          end_vert_for_chunk = start_vert_for_chunk + CHUNK_SIZE;
          m_next_vert += CHUNK_SIZE;
        } else {
          // should do all!
          start_vert_for_chunk = m_next_vert;
          end_vert_for_chunk = end_vert_in_tree;
          complete_tree = true;
        }

        u32 upload_size =
            (end_vert_for_chunk - start_vert_for_chunk) * sizeof(tfrag3::PreloadedVertex);
        upload_to_buffer(GL_ARRAY_BUFFER, data.lev_data->tfrag_vertex_data[m_next_geo][m_next_tree],
                         start_vert_for_chunk * sizeof(tfrag3::PreloadedVertex), upload_size,
                         tree.unpacked.vertices.data() + start_vert_for_chunk);
        uploaded_bytes += upload_size;
      }

      if (complete_tree) {
        unique_buffers++;
        // and move on to next tree
        m_next_vert = 0;
        m_next_tree++;
        if (m_next_tree >= data.lev_data->level->tfrag_trees[m_next_geo].size()) {
          m_next_tree = 0;
          m_next_geo++;
          if (m_next_geo >= tfrag3::TFRAG_GEOS) {
            m_next_tree = true;
            m_next_tree = 0;
            m_next_geo = 0;
            m_next_vert = 0;
            m_done = true;
            return true;
          }
        }
        return false;
      }

      if (timer.getMs() > LOAD_BUDGET || (uploaded_bytes / 1024) > MAX_STAGE_UPLOAD_KB) {
        return false;
      }
    }
  }

  void reset() override {
    m_done = false;
    m_opengl_created = false;
    m_next_geo = 0;
    m_next_tree = 0;
    m_next_vert = 0;
  }

 private:
  bool m_done = false;
  bool m_opengl_created = false;
  u32 m_next_geo = 0;
  u32 m_next_tree = 0;
  u32 m_next_vert = 0;
};

class ShrubLoadStage : public LoaderStage {
 public:
  ShrubLoadStage() : LoaderStage("shrub") {}
  bool run(Timer& timer, LoaderInput& data) override {
    if (m_done) {
      return true;
    }

    if (data.lev_data->level->shrub_trees.empty()) {
      m_done = true;
      return true;
    }

    if (!m_opengl_created) {
      for (auto& in_tree : data.lev_data->level->shrub_trees) {
        // FIX 33: pooled buffers (see GpuBufferPool.h).
        GLuint& tree_out = data.lev_data->shrub_vertex_data.emplace_back();
        tree_out = data.buffers->acquire(
            GL_ARRAY_BUFFER,
            (GLsizeiptr)in_tree.unpacked.vertices.size() * sizeof(tfrag3::ShrubGpuVertex));
      }
      m_opengl_created = true;
      return false;
    }

    constexpr u32 CHUNK_SIZE = STAGE_VERT_CHUNK;
    u32 uploaded_bytes = 0;

    while (true) {
      const auto& tree = data.lev_data->level->shrub_trees[m_next_tree];
      u32 end_vert_in_tree = tree.unpacked.vertices.size();
      // the number of vertices we'd need to finish the tree right now
      size_t num_verts_left_in_tree = end_vert_in_tree - m_next_vert;
      size_t start_vert_for_chunk;
      size_t end_vert_for_chunk;

      bool complete_tree;

      if (num_verts_left_in_tree > CHUNK_SIZE) {
        complete_tree = false;
        // should only do partial
        start_vert_for_chunk = m_next_vert;
        end_vert_for_chunk = start_vert_for_chunk + CHUNK_SIZE;
        m_next_vert += CHUNK_SIZE;
      } else {
        // should do all!
        start_vert_for_chunk = m_next_vert;
        end_vert_for_chunk = end_vert_in_tree;
        complete_tree = true;
      }

      u32 upload_size =
          (end_vert_for_chunk - start_vert_for_chunk) * sizeof(tfrag3::ShrubGpuVertex);
      upload_to_buffer(GL_ARRAY_BUFFER, data.lev_data->shrub_vertex_data[m_next_tree],
                       start_vert_for_chunk * sizeof(tfrag3::ShrubGpuVertex), upload_size,
                       tree.unpacked.vertices.data() + start_vert_for_chunk);
      uploaded_bytes += upload_size;

      if (complete_tree) {
        // and move on to next tree
        m_next_vert = 0;
        m_next_tree++;
        if (m_next_tree >= data.lev_data->level->shrub_trees.size()) {
          m_done = true;
          return true;
        }
      }

      if (timer.getMs() > LOAD_BUDGET || (uploaded_bytes / 1024) > MAX_STAGE_UPLOAD_KB) {
        return false;
      }
    }
  }

  void reset() override {
    m_done = false;
    m_opengl_created = false;
    m_next_tree = 0;
    m_next_vert = 0;
  }

 private:
  bool m_done = false;
  bool m_opengl_created = false;
  u32 m_next_tree = 0;
  u32 m_next_vert = 0;
};

class TieLoadStage : public LoaderStage {
 public:
  TieLoadStage() : LoaderStage("tie") {}
  bool run(Timer& timer, LoaderInput& data) override {
    if (m_done) {
      return true;
    }

    if (data.lev_data->level->tie_trees.front().empty()) {
      m_done = true;
      return true;
    }

    if (!m_opengl_created) {
      auto evt = scoped_prof("tie-opengl-create");
      for (int geo = 0; geo < tfrag3::TIE_GEOS; geo++) {
        auto& in_trees = data.lev_data->level->tie_trees[geo];
        for (auto& in_tree : in_trees) {
          // FIX 33: pooled buffers (see GpuBufferPool.h).
          LevelData::TieOpenGL& tree_out = data.lev_data->tie_data[geo].emplace_back();
          tree_out.vertex_buffer = data.buffers->acquire(
              GL_ARRAY_BUFFER,
              (GLsizeiptr)in_tree.unpacked.vertices.size() * sizeof(tfrag3::PreloadedVertex));
          tree_out.index_buffer = data.buffers->acquire(
              GL_ELEMENT_ARRAY_BUFFER,
              (GLsizeiptr)in_tree.unpacked.indices.size() * sizeof(u32));
        }
      }
      m_opengl_created = true;
      return false;
    }

    if (!m_verts_done) {
      auto evt = scoped_prof("tie-verts");
      constexpr u32 CHUNK_SIZE = STAGE_VERT_CHUNK;
      u32 uploaded_bytes = 0;

      while (true) {
        const auto& tree = data.lev_data->level->tie_trees[m_next_geo][m_next_tree];
        u32 end_vert_in_tree = tree.unpacked.vertices.size();
        // the number of vertices we'd need to finish the tree right now
        size_t num_verts_left_in_tree = end_vert_in_tree - m_next_vert;
        size_t start_vert_for_chunk;
        size_t end_vert_for_chunk;

        bool complete_tree;

        if (num_verts_left_in_tree > CHUNK_SIZE) {
          complete_tree = false;
          // should only do partial
          start_vert_for_chunk = m_next_vert;
          end_vert_for_chunk = start_vert_for_chunk + CHUNK_SIZE;
          m_next_vert += CHUNK_SIZE;
        } else {
          // should do all!
          start_vert_for_chunk = m_next_vert;
          end_vert_for_chunk = end_vert_in_tree;
          complete_tree = true;
        }

        u32 upload_size =
            (end_vert_for_chunk - start_vert_for_chunk) * sizeof(tfrag3::PreloadedVertex);
        {
          auto bsd = scoped_prof(fmt::format("buffer-{}k", upload_size / 1024).c_str());
          upload_to_buffer(GL_ARRAY_BUFFER,
                           data.lev_data->tie_data[m_next_geo][m_next_tree].vertex_buffer,
                           start_vert_for_chunk * sizeof(tfrag3::PreloadedVertex), upload_size,
                           tree.unpacked.vertices.data() + start_vert_for_chunk);
        }

        uploaded_bytes += upload_size;

        if (complete_tree) {
          // and move on to next tree
          m_next_vert = 0;
          m_next_tree++;
          if (m_next_tree >= data.lev_data->level->tie_trees[m_next_geo].size()) {
            m_next_tree = 0;
            m_next_geo++;
            while (m_next_geo < tfrag3::TIE_GEOS &&
                   data.lev_data->level->tie_trees[m_next_geo].empty()) {
              m_next_geo++;
            }
            if (m_next_geo >= tfrag3::TIE_GEOS) {
              m_verts_done = true;
              m_next_tree = 0;
              m_next_geo = 0;
              m_next_vert = 0;
              return false;
            }
          }
        }

        if (timer.getMs() > LOAD_BUDGET || (uploaded_bytes / 1024) > MAX_STAGE_UPLOAD_KB) {
          return false;
        }
      }
    }

    if (!m_wind_indices_done) {
      auto evt = scoped_prof("tie-wind");
      bool abort = false;
      for (; m_next_geo < tfrag3::TIE_GEOS; m_next_geo++) {
        auto& geo_trees = data.lev_data->level->tie_trees[m_next_geo];
        for (; m_next_tree < geo_trees.size(); m_next_tree++) {
          if (abort) {
            return false;
          }
          auto& in_tree = geo_trees[m_next_tree];
          auto& out_tree = data.lev_data->tie_data[m_next_geo][m_next_tree];
          size_t wind_idx_buffer_len = 0;
          for (auto& draw : in_tree.instanced_wind_draws) {
            wind_idx_buffer_len += draw.vertex_index_stream.size();
          }
          if (wind_idx_buffer_len > 0) {
            out_tree.has_wind = true;
            out_tree.wind_indices = data.buffers->acquire(
                GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)wind_idx_buffer_len * sizeof(u32));
            std::vector<u32> temp;
            temp.resize(wind_idx_buffer_len);
            u32 off = 0;
            for (auto& draw : in_tree.instanced_wind_draws) {
              memcpy(temp.data() + off, draw.vertex_index_stream.data(),
                     draw.vertex_index_stream.size() * sizeof(u32));
              off += draw.vertex_index_stream.size();
            }

            upload_to_buffer(GL_ELEMENT_ARRAY_BUFFER, out_tree.wind_indices, 0,
                             wind_idx_buffer_len * sizeof(u32), temp.data());
            abort = true;
          }
        }
        m_next_tree = 0;
      }

      m_wind_indices_done = true;
      m_next_geo = 0;
      m_next_vert = 0;
      m_next_tree = 0;

      if (timer.getMs() > LOAD_BUDGET) {
        return false;
      }
    }

    if (!m_indices_done) {
      auto evt = scoped_prof("tie-ind");
      constexpr u32 CHUNK_SIZE = STAGE_INDEX_CHUNK;
      u32 uploaded_bytes = 0;

      while (true) {
        const auto& tree = data.lev_data->level->tie_trees[m_next_geo][m_next_tree];
        u32 end_ind_in_tree = tree.unpacked.indices.size();
        // the number of indices we'd need to finish the tree right now
        size_t num_inds_left_in_tree = end_ind_in_tree - m_next_vert;
        size_t start_ind_for_chunk;
        size_t end_ind_for_chunk;

        bool complete_tree;

        if (num_inds_left_in_tree > CHUNK_SIZE) {
          complete_tree = false;
          // should only do partial
          start_ind_for_chunk = m_next_vert;
          end_ind_for_chunk = start_ind_for_chunk + CHUNK_SIZE;
          m_next_vert += CHUNK_SIZE;
        } else {
          // should do all!
          start_ind_for_chunk = m_next_vert;
          end_ind_for_chunk = end_ind_in_tree;
          complete_tree = true;
        }

        u32 upload_size = (end_ind_for_chunk - start_ind_for_chunk) * sizeof(u32);
        upload_to_buffer(GL_ELEMENT_ARRAY_BUFFER,
                         data.lev_data->tie_data[m_next_geo][m_next_tree].index_buffer,
                         start_ind_for_chunk * sizeof(u32), upload_size,
                         tree.unpacked.indices.data() + start_ind_for_chunk);
        uploaded_bytes += upload_size;

        if (complete_tree) {
          // and move on to next tree
          m_next_vert = 0;
          m_next_tree++;
          if (m_next_tree >= data.lev_data->level->tie_trees[m_next_geo].size()) {
            m_next_tree = 0;
            m_next_geo++;
            while (m_next_geo < tfrag3::TIE_GEOS &&
                   data.lev_data->level->tie_trees[m_next_geo].empty()) {
              m_next_geo++;
            }
            if (m_next_geo >= tfrag3::TIE_GEOS) {
              m_indices_done = true;
              m_next_tree = 0;
              m_next_geo = 0;
              m_next_vert = 0;
              m_done = true;
              return true;
            }
          }
        }

        if (timer.getMs() > LOAD_BUDGET || (uploaded_bytes / 1024) > MAX_STAGE_UPLOAD_KB) {
          return false;
        }
      }
    }

    return false;
  }

  void reset() override {
    m_done = false;
    m_opengl_created = false;
    m_next_geo = 0;
    m_next_tree = 0;
    m_next_vert = 0;
    m_verts_done = false;
    m_indices_done = false;
    m_wind_indices_done = false;
  }

 private:
  bool m_done = false;
  bool m_opengl_created = false;
  bool m_verts_done = false;
  bool m_indices_done = false;
  bool m_wind_indices_done = false;
  u32 m_next_geo = 0;
  u32 m_next_tree = 0;
  u32 m_next_vert = 0;
};

class CollideLoaderStage : public LoaderStage {
 public:
  CollideLoaderStage() : LoaderStage("collide") {}
  bool run(Timer& /*timer*/, LoaderInput& data) override {
    if (m_done) {
      return true;
    }
    if (!m_opengl_created) {
      // FIX 33: pooled buffers (see GpuBufferPool.h).
      data.lev_data->collide_vertices = data.buffers->acquire(
          GL_ARRAY_BUFFER, (GLsizeiptr)data.lev_data->level->collision.vertices.size() *
                               sizeof(tfrag3::CollisionMesh::Vertex));
      m_opengl_created = true;
      return false;
    }

    u32 start = m_vtx;
    u32 end =
        std::min((u32)data.lev_data->level->collision.vertices.size(), start + STAGE_VERT_CHUNK);
    upload_to_buffer(GL_ARRAY_BUFFER, data.lev_data->collide_vertices,
                     start * sizeof(tfrag3::CollisionMesh::Vertex),
                     (end - start) * sizeof(tfrag3::CollisionMesh::Vertex),
                     data.lev_data->level->collision.vertices.data() + start);
    m_vtx = end;

    if (m_vtx == data.lev_data->level->collision.vertices.size()) {
      m_done = true;
      return true;
    } else {
      return false;
    }
  }
  void reset() override {
    m_opengl_created = false;
    m_vtx = 0;
    m_done = false;
  }

 private:
  bool m_opengl_created = false;
  u32 m_vtx = 0;
  bool m_done = false;
};

class StallLoaderStage : public LoaderStage {
 public:
  StallLoaderStage() : LoaderStage("stall") {}
  bool run(Timer&, LoaderInput& /*data*/) override {
    m_count++;
    if (m_count > 10) {
      return true;
    }
    return false;
  }

  void reset() override { m_count = 0; }

 private:
  int m_count = 0;
};

class HfragLoaderStage : public LoaderStage {
 public:
  HfragLoaderStage() : LoaderStage("hfrag") {}
  void reset() override {
    m_done = false;
    m_opengl = false;
    m_vtx_uploaded = false;
    m_idx = 0;
  }

  bool run(Timer&, LoaderInput& data) override {
    if (m_done) {
      return true;
    }

    if (!m_opengl) {
      // FIX 33: pooled buffers (see GpuBufferPool.h).
      data.lev_data->hfrag_indices = data.buffers->acquire(
          GL_ELEMENT_ARRAY_BUFFER,
          (GLsizeiptr)data.lev_data->level->hfrag.indices.size() * sizeof(u32));

      data.lev_data->hfrag_vertices = data.buffers->acquire(
          GL_ARRAY_BUFFER, (GLsizeiptr)data.lev_data->level->hfrag.vertices.size() *
                               sizeof(tfrag3::HfragmentVertex));
      m_opengl = true;
    }

    if (!m_vtx_uploaded) {
      u32 start = m_idx;
      m_idx = std::min(start + STAGE_VERT_CHUNK,
                      (u32)data.lev_data->level->hfrag.indices.size());
      upload_to_buffer(GL_ARRAY_BUFFER, data.lev_data->hfrag_indices, start * sizeof(u32),
                       (m_idx - start) * sizeof(u32),
                       data.lev_data->level->hfrag.indices.data() + start);
      if (m_idx != data.lev_data->level->hfrag.indices.size()) {
        return false;
      } else {
        m_idx = 0;
        m_vtx_uploaded = true;
      }
    }

    u32 start = m_idx;
    m_idx = std::min(start + STAGE_VERT_CHUNK, (u32)data.lev_data->level->hfrag.vertices.size());
    upload_to_buffer(GL_ARRAY_BUFFER, data.lev_data->hfrag_vertices,
                     start * sizeof(tfrag3::HfragmentVertex),
                     (m_idx - start) * sizeof(tfrag3::HfragmentVertex),
                     data.lev_data->level->hfrag.vertices.data() + start);

    if (m_idx != data.lev_data->level->hfrag.vertices.size()) {
      return false;
    } else {
      m_done = true;
      return true;
    }
    return true;
  }

 private:
  bool m_done = false;
  bool m_opengl = false;
  bool m_vtx_uploaded = false;
  u32 m_idx = 0;
};

MercLoaderStage::MercLoaderStage() : LoaderStage("merc") {}
void MercLoaderStage::reset() {
  m_done = false;
  m_opengl = false;
  m_vtx_uploaded = false;
  m_idx = 0;
}

bool MercLoaderStage::run(Timer& /*timer*/, LoaderInput& data) {
  if (m_done) {
    return true;
  }

  if (!m_opengl) {
    // FIX 33: pooled buffers (see GpuBufferPool.h).
    data.lev_data->merc_indices = data.buffers->acquire(
        GL_ELEMENT_ARRAY_BUFFER,
        (GLsizeiptr)data.lev_data->level->merc_data.indices.size() * sizeof(u32));

    data.lev_data->merc_vertices = data.buffers->acquire(
        GL_ARRAY_BUFFER,
        (GLsizeiptr)data.lev_data->level->merc_data.vertices.size() * sizeof(tfrag3::MercVertex));
    m_opengl = true;
  }

  if (!m_vtx_uploaded) {
    u32 start = m_idx;
    m_idx = std::min(start + STAGE_VERT_CHUNK,
                    (u32)data.lev_data->level->merc_data.indices.size());
    upload_to_buffer(GL_ARRAY_BUFFER, data.lev_data->merc_indices, start * sizeof(u32),
                     (m_idx - start) * sizeof(u32),
                     data.lev_data->level->merc_data.indices.data() + start);
    if (m_idx != data.lev_data->level->merc_data.indices.size()) {
      return false;
    } else {
      m_idx = 0;
      m_vtx_uploaded = true;
    }
  }

  u32 start = m_idx;
  m_idx = std::min(start + STAGE_VERT_CHUNK, (u32)data.lev_data->level->merc_data.vertices.size());
  upload_to_buffer(GL_ARRAY_BUFFER, data.lev_data->merc_vertices,
                   start * sizeof(tfrag3::MercVertex),
                   (m_idx - start) * sizeof(tfrag3::MercVertex),
                   data.lev_data->level->merc_data.vertices.data() + start);

  if (m_idx != data.lev_data->level->merc_data.vertices.size()) {
    return false;
  } else {
    m_done = true;
    for (auto& model : data.lev_data->level->merc_data.models) {
      data.lev_data->merc_model_lookup[model.name] = &model;
      (*data.mercs)[model.name].push_back({&model, data.lev_data->load_id, data.lev_data});
    }
    return true;
  }
  return true;
}

std::vector<std::unique_ptr<LoaderStage>> make_loader_stages() {
  std::vector<std::unique_ptr<LoaderStage>> ret;
  ret.push_back(std::make_unique<TieLoadStage>());
  ret.push_back(std::make_unique<TextureLoaderStage>());
  ret.push_back(std::make_unique<TfragLoadStage>());
  ret.push_back(std::make_unique<ShrubLoadStage>());
  ret.push_back(std::make_unique<CollideLoaderStage>());
  ret.push_back(std::make_unique<MercLoaderStage>());
  ret.push_back(std::make_unique<HfragLoaderStage>());
  ret.push_back(std::make_unique<StallLoaderStage>());
  return ret;
}
