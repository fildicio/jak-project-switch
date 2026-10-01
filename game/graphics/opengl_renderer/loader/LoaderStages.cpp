#include <algorithm>
#include <vector>
#include <unordered_map>
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

int mipq_process(int max_count) {
  int done = 0;
  glActiveTexture(GL_TEXTURE0);
  while (done < max_count && !g_mip_queue.empty()) {
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

u64 add_texture(TexturePool& pool, const tfrag3::Texture& tex, bool is_common) {
  GLuint gl_tex;
  glActiveTexture(GL_TEXTURE0);
  glGenTextures(1, &gl_tex);
  glBindTexture(GL_TEXTURE_2D, gl_tex);
#ifdef __SWITCH__
  Timer tex_upload_timer;
  tex_upload_timer.start();
#endif
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
  if ((u64)tex.w * tex.h != tex.data.size()) {
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
        ld.textures.push_back(add_texture(*data.tex_pool, tex, false));
        bytes_this_run += tex.w * tex.h * 4;
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
        const int max_tex_this_dispatch =
            std::clamp<int>(g_loader_budget.tex_bytes / (64 * 1024), 4, 20);
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
  }

 private:
  GLuint m_cur_tex = 0;
  bool m_cur_allocated = false;
  int m_cur_row = 0;
  bool m_logged_stats = false;
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
