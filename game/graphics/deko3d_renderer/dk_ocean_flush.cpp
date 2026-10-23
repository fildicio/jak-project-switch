// T95 (AI-assisted): deko3d ocean GPU half. Port of the GL draws:
//   - CommonOceanRenderer::flush_near  (3 buckets, depth GEQUAL no-write)
//   - CommonOceanRenderer::flush_mid   (2 buckets, depth ALWAYS + write,
//     envmap indices already reversed by the driver, DST_ALPHA/ONE blend)
//   - OceanTexture::flush              (the 128x128 ocean-texture generation,
//     rendered into a dktexgpu render target registered in the pool at
//     OCEAN_TEX_TBP_JAK1 = 8160 - the T82 eye pattern)
// The ocean uses blend equations the pipeline table has no entry for (the
// ZERO/ONE alpha-only pass and the GL DST_ALPHA,ONE,ONE,ZERO envmap pass), so
// this file binds its own DkDepthStencilState / DkBlendState and picks the
// sampler with dkpipe::find_sampler. Called from present_frame's world
// recorder; never submits. FIX 112 (AI-assisted): the result texture at tbp
// 8160 carries a full 8-level mip chain regenerated every frame on the GPU
// (GL OceanTexture::make_texture_with_mipmaps), replacing the T95 single-mip
// limitation that caused the horizon envmap line + minification sparkle.
// FIX 113 (AI-assisted): the texgen samples the envmap LINEAR_MIPMAP_LINEAR
// like GL's OceanTexture::flush (single-mip envmap images clamp to L0, so this
// is a no-op there), and the T99 probe additionally reads back the RESULT
// texture L0+L3 - the image the body draws actually sample - so a silently
// failed record_mip_chain shows up as "result black, temp healthy" in the log
// instead of flat/black water with no clue.
#include "game/graphics/deko3d_renderer/dk_ocean_driver.h"

#if defined(__SWITCH__)
#include <deko3d.h>
#include <string.h>

#include "common/dma/gs.h"
#include "common/versions/versions.h"
#include "game/graphics/deko3d_renderer/DirectDk.h"
#include "game/graphics/deko3d_renderer/dk_state.h"
#include "game/graphics/deko3d_renderer/dk_texture_gpu.h"
#include "game/graphics/pipelines/dk_device.h"
#include "game/graphics/pipelines/dk_device_internal.h"
#include "game/graphics/texture/TexturePool.h"

// switch_run_logf lives in run_log.h, which cannot be included next to <deko3d.h>.
void switch_run_logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

namespace dkocean {
namespace {

constexpr uint32_t kTexImages = 2048;  // dktexgpu descriptor slots
constexpr uint32_t kUboSize = 0x100;   // one OceanCommonParams per bucket value
constexpr uint32_t kOceanTexSize = 128;
constexpr uint32_t kOceanTexMips = 8;  // FIX 112: GL OceanTexture::NUM_MIPS
constexpr int kOceanTexTbpJak1 = 8160;  // GL OCEAN_TEX_TBP_JAK1

// std140 mirror of OceanCommonVert (binding 6) / OceanCommonFrag (binding 7):
// one 32-byte buffer, both stages. GL's fog_color uniform is
// (rgb / 255, intensity / 255); color_mult / alpha_mult were dead and dropped.
struct OceanCommonParams {
  float fog_color[4];
  int bucket;
  int pad[3];
};
static_assert(sizeof(OceanCommonParams) == 32);

// FIX 112: std140 mirror of the ocean_mipmap fragment block (binding 7).
struct OceanMipParams {
  float params[4];  // x = alpha_intensity (GL: max(0, 1 - 0.51*i))
};
static_assert(sizeof(OceanMipParams) == 16);

// GL CommonRecorder::Vertex (32 B): xyz f3 @0, rgba unorm u8x4 @12,
// stq f3 @16, fog raw u8 @28 (glVertexAttribIPointer -> Uint).
const DkVtxAttribState kCommonAttribs[] = {
    {0, 0, 0, DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
    {0, 0, 12, DkVtxAttribSize_4x8, DkVtxAttribType_Unorm, 0},
    {0, 0, 16, DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
    {0, 0, 28, DkVtxAttribSize_1x8, DkVtxAttribType_Uint, 0},
};
// GL OceanTexture PC draw: static positions (buffer 0, f2) + dynamic
// {st f2 @0, rgba unorm u8x4 @8} (buffer 1, 16 B stride).
const DkVtxAttribState kTexAttribs[] = {
    {0, 0, 0, DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
    {1, 0, 8, DkVtxAttribSize_4x8, DkVtxAttribType_Unorm, 0},
    {1, 0, 0, DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
};

// FlushStats comes from dk_ocean_driver.h (plain header) now.
FlushStats s_stats;

// The ocean texture (FIX 112): s_texgen_rt_id is the 1-level temp the texgen
// renders into every frame (GL m_temp_texture); s_ocean_tex_id is the 8-level
// result registered at tbp 8160 that the body draws sample (GL
// m_result_texture). record_mip_chain copies temp into all result levels with
// GL's alpha fade. The temp needs no pool entry - only the mip pass reads it.
uint32_t s_ocean_tex_id = 0;
uint32_t s_texgen_rt_id = 0;
GpuTexture* s_ocean_tex_gpu = nullptr;

// FIX 112: the mip pass's static fullscreen quad (pos f2 + st f2, 4 verts).
uint64_t s_mip_quad_gpu = 0;

// Static (never-changing) texture-gen buffers: 2112 positions + the restart
// index list. Acquired once from the buffer pool and kept for the device's
// lifetime (the pool frees them at teardown).
uint64_t s_static_pos_gpu = 0, s_static_idx_gpu = 0;
uint32_t s_static_idx_count = 0;
bool s_static_failed = false;

// T98 debug probe (AI-assisted): one-shot readback of the 128x128 ocean-texture
// render target. The copy is recorded in record_mid_far right after the texgen
// draw of frame N; build_frame of frame N+3 (two present-fences later) scans the
// texels and logs the average color + black/magenta counts. This tells us what
// the mid/near passes actually sample (correct waves vs black / purple garbage /
// a thin blue band) without needing a screenshot. The 64 KB pool buffer is kept
// for the run (one-shot). Remove once the ocean visuals are diagnosed.
dkpool::PoolBuffer s_probe;
uint32_t s_probe_at = 0;    // s_build when the copy was recorded (0 = idle)
bool s_probe_armed = false;
// T99: the one-shot probe of T98 fired while the ocean bucket was still idle
// (s_build counts from the first chain, not from the first ocean draw). Arm on
// real activity instead: after the texgen GPU draw has run N times, and repeat
// every so often so we get snapshots during actual gameplay.
uint32_t s_probe_round = 0;         // snapshots taken so far
uint32_t s_probe_next = 240;        // arm once tex_gen_draws reaches this
constexpr uint32_t kProbeRounds = 5;
constexpr uint32_t kProbeStride = 900;  // ~15 s of gameplay between snapshots
uint32_t s_probe_draws = 0;         // tex_gen_draws at arm time
// FIX 113: the same probe round also copies the RESULT texture (L0 + L3).
// The body draws sample the result, not the temp: a healthy temp average with
// black mips means record_mip_chain never ran (or failed silently) and the
// ocean would render flat/black while every other number looks fine.
dkpool::PoolBuffer s_probe_mip0;
dkpool::PoolBuffer s_probe_mip3;

// ---- per-frame GPU ring, 3 slots like DirectDk/SpriteDk ----
constexpr uint32_t kSlots = 3;
struct Slot {
  dkpool::PoolBuffer pb;
  uint32_t used = 0;
  uint32_t want = 0;
};
Slot s_slot[kSlots];
uint32_t s_build = 0;

Slot* slot_begin(uint32_t need_now) {
  Slot* s = &s_slot[s_build % kSlots];
  s->used = 0;
  s->want = std::max(s->want, need_now);
  dkpool::BufferPool* pool = dkdev::buffer_pool();
  const uint32_t need = std::max<uint32_t>(128u << 10, s->want + s->want / 2);
  if (pool && (!s->pb.ok() || s->pb.capacity < s->want)) {
    if (s->pb.ok()) {
      pool->release(s->pb.id);
    }
    s->pb = pool->acquire(need);
    switch_run_logf("[dk] ocean ring slot %u: %u KB %s", s_build % kSlots, need >> 10,
                    s->pb.ok() ? "ok" : "FAILED");
  }
  s->want = 0;
  return s;
}

uint8_t* slot_alloc(Slot* s, uint32_t bytes, uint64_t* gpu) {
  bytes = (bytes + 0xff) & ~0xffu;
  s->want += bytes;
  if (!s->pb.ok() || s->used + bytes > s->pb.capacity) {
    return nullptr;
  }
  uint8_t* p = s->pb.cpu + s->used;
  *gpu = s->pb.gpu + s->used;
  s->used += bytes;
  return p;
}

// One half (near or mid) of the body data, uploaded at build time.
struct BodyDraws {
  uint64_t vert_gpu = 0;
  uint32_t vert_bytes = 0;
  uint64_t idx_gpu[CommonRecorder::NUM_BUCKETS] = {};
  uint32_t idx_count[CommonRecorder::NUM_BUCKETS] = {};
  uint32_t ocean_slot = UINT32_MAX;   // dktexgpu slot at tbp 8160
  uint32_t envmap_slot = UINT32_MAX;  // adgif envmap, UINT32_MAX = missing
  uint32_t verts = 0;
  bool valid = false;
};
BodyDraws s_near_draws, s_mid_draws;

// The texture-gen dynamic vertex data of this frame.
uint64_t s_texdyn_gpu = 0;
uint32_t s_texdyn_bytes = 0;
bool s_texdyn_valid = false;

uint8_t s_fog_rgba[4] = {0, 0, 0, 0};

uint32_t slot_of_or_max(uint32_t id) {
  return id == 0 ? UINT32_MAX : dktexgpu::slot_of(id);
}

void build_body(BodyDraws& out, CommonRecorder& rec) {
  out = BodyDraws();
  const uint32_t verts = rec.next_free_vertex();
  if (verts == 0) {
    return;
  }
  uint32_t idx_bytes = 0;
  for (int b = 0; b < CommonRecorder::NUM_BUCKETS; b++) {
    idx_bytes += rec.next_free_index(b) * 4;
  }
  Slot* ring = slot_begin(verts * 32 + idx_bytes);
  uint64_t gpu = 0;
  uint8_t* p = slot_alloc(ring, verts * 32, &gpu);
  if (p == nullptr) {
    s_stats.dropped++;
    return;
  }
  memcpy(p, rec.vertices_data(), (size_t)verts * 32);
  out.vert_gpu = gpu;
  out.vert_bytes = verts * 32;
  out.verts = verts;
  for (int b = 0; b < CommonRecorder::NUM_BUCKETS; b++) {
    const uint32_t n = rec.next_free_index(b);
    if (n == 0) {
      continue;
    }
    p = slot_alloc(ring, n * 4, &gpu);
    if (p == nullptr) {
      s_stats.dropped++;
      return;
    }
    memcpy(p, rec.indices_data(b), (size_t)n * 4);
    out.idx_gpu[b] = gpu;
    out.idx_count[b] = n;
  }
  out.ocean_slot = slot_of_or_max(s_ocean_tex_id);
  LookupFn lk = lookup();
  if (lk != nullptr && rec.envmap_tex() != 0) {
    out.envmap_slot = slot_of_or_max(lk(rec.envmap_tex(), false));
  }
  out.valid = out.ocean_slot != UINT32_MAX;
}

}  // namespace

// T95: create the ocean-texture render target and register it in the pool at
// the fixed tbp the body draws sample (GL OceanTexture::init_textures).
// FIX 112: GL renders the texgen into a temp texture and keeps a separate
// 8-level result texture; the dk port now mirrors that split exactly.
void init_textures(TexturePool* pool, GameVersion version) {
  if (pool == nullptr || s_ocean_tex_id != 0) {
    return;
  }
  s_texgen_rt_id = dktexgpu::create_render_target(kOceanTexSize, kOceanTexSize);
  if (s_texgen_rt_id != 0) {
    s_ocean_tex_id =
        dktexgpu::create_render_target_mipped(kOceanTexSize, kOceanTexSize, kOceanTexMips);
  }
  if (s_ocean_tex_id == 0) {
    switch_run_logf("[dk] ocean tex target FAILED (store full / memblock)");
    s_texgen_rt_id = 0;  // keep last-frame fallback; image memory is not reclaimable
    return;
  }
  TextureInput in;
  in.gpu_texture = s_ocean_tex_id;
  in.w = kOceanTexSize;
  in.h = kOceanTexSize;
  in.debug_name = "DK-OCEAN-TEX";
  in.id = pool->allocate_pc_port_texture(version);
  s_ocean_tex_gpu = pool->give_texture_and_load_to_vram(in, kOceanTexTbpJak1);
  switch_run_logf("[dk] ocean tex target at tbp %d: id %u (mips %u, texgen temp %u)",
                  kOceanTexTbpJak1, s_ocean_tex_id, kOceanTexMips, s_texgen_rt_id);
}

// T95: render-thread build step (after the captures, before present). Copies
// the decoded vertices/indices into the GPU ring and resolves texture slots.
void build_frame(const uint8_t fog_rgba[4]) {
  memcpy(s_fog_rgba, fog_rgba, 4);
  s_near_draws = BodyDraws();
  s_mid_draws = BodyDraws();
  s_texdyn_valid = false;
  s_build++;

  build_body(s_near_draws, near_recorder());
  build_body(s_mid_draws, mid_recorder());

  // static texture-gen buffers (positions + restart index list), once
  const TextureGen& tg = texture_gen();
  if (!s_static_failed && s_static_pos_gpu == 0 && tg.positions().size() == 2112 &&
      !tg.indices().empty()) {
    dkpool::BufferPool* pool = dkdev::buffer_pool();
    if (pool != nullptr) {
      dkpool::PoolBuffer pos = pool->acquire((uint32_t)(tg.positions().size() * 8));
      dkpool::PoolBuffer idx = pool->acquire((uint32_t)(tg.indices().size() * 4));
      if (pos.ok() && idx.ok()) {
        memcpy(pos.cpu, tg.positions().data(), tg.positions().size() * 8);
        memcpy(idx.cpu, tg.indices().data(), tg.indices().size() * 4);
        s_static_pos_gpu = pos.gpu;
        s_static_idx_gpu = idx.gpu;
        s_static_idx_count = (uint32_t)tg.indices().size();
      } else {
        s_static_failed = true;
        if (pos.ok()) {
          pool->release(pos.id);
        }
        if (idx.ok()) {
          pool->release(idx.id);
        }
      }
    } else {
      s_static_failed = true;
    }
  }

  // FIX 112: the mip pass's fullscreen quad (once).
  if (!s_static_failed && s_mip_quad_gpu == 0) {
    dkpool::BufferPool* pool = dkdev::buffer_pool();
    if (pool != nullptr) {
      static const float kMipQuad[16] = {-1, -1, 0, 0, -1, 1, 0, 1,
                                         1,  -1, 1, 0, 1,  1, 1, 1};
      dkpool::PoolBuffer q = pool->acquire(sizeof(kMipQuad));
      if (q.ok()) {
        memcpy(q.cpu, kMipQuad, sizeof(kMipQuad));
        s_mip_quad_gpu = q.gpu;
      }
    }
  }

  // this frame's dynamic texture-gen vertices
  if (tg.output_valid()) {
    Slot* ring = slot_begin((uint32_t)(tg.dynamic_vertices().size() * 16));
    uint64_t gpu = 0;
    uint8_t* p = slot_alloc(ring, (uint32_t)(tg.dynamic_vertices().size() * 16), &gpu);
    if (p != nullptr) {
      memcpy(p, tg.dynamic_vertices().data(), tg.dynamic_vertices().size() * 16);
      s_texdyn_gpu = gpu;
      s_texdyn_bytes = (uint32_t)(tg.dynamic_vertices().size() * 16);
      s_texdyn_valid = true;
    } else {
      s_stats.dropped++;
    }
  }

  // T99 debug probe: analyze the RT copy once two present-fences have passed,
  // and dump the CPU-side decode data of the same passes. Periodic (5 rounds),
  // armed on actual texgen GPU draws so it samples real gameplay.
  if (s_probe_armed && s_build > s_probe_at + 2) {
    s_probe_armed = false;
    const uint8_t* px = s_probe.cpu;
    const uint32_t n = kOceanTexSize * kOceanTexSize;
    uint64_t ar = 0, ag = 0, ab = 0, aa = 0;
    uint32_t black = 0, magenta = 0;
    for (uint32_t i = 0; i < n; i++) {
      ar += px[i * 4];
      ag += px[i * 4 + 1];
      ab += px[i * 4 + 2];
      aa += px[i * 4 + 3];
      if (px[i * 4] + px[i * 4 + 1] + px[i * 4 + 2] < 12) {
        black++;
      }
      if (px[i * 4] > 200 && px[i * 4 + 2] > 200 && px[i * 4 + 1] < 80) {
        magenta++;
      }
    }
    switch_run_logf("[dk] ocean probe #%u RT @f%u d%u: avg rgba %u,%u,%u,%u | black %u/%u "
                    "magenta %u",
                    s_probe_round + 1, s_probe_at, s_probe_draws, (unsigned)(ar / n),
                    (unsigned)(ag / n), (unsigned)(ab / n), (unsigned)(aa / n), black, n,
                    magenta);
    const uint8_t* r0 = px;
    const uint8_t* r32 = px + 32 * kOceanTexSize * 4;
    const uint8_t* r64 = px + 64 * kOceanTexSize * 4;
    const uint8_t* r96 = px + 96 * kOceanTexSize * 4;
    switch_run_logf("[dk] ocean probe RT texels r0 %02x%02x%02x%02x,%02x%02x%02x%02x | r32 "
                    "%02x%02x%02x%02x,%02x%02x%02x%02x | r64 %02x%02x%02x%02x,%02x%02x%02x%02x | "
                    "r96 %02x%02x%02x%02x,%02x%02x%02x%02x",
                    r0[0], r0[1], r0[2], r0[3], r0[4], r0[5], r0[6], r0[7], r32[0], r32[1],
                    r32[2], r32[3], r32[4], r32[5], r32[6], r32[7], r64[0], r64[1], r64[2],
                    r64[3], r64[4], r64[5], r64[6], r64[7], r96[0], r96[1], r96[2], r96[3],
                    r96[4], r96[5], r96[6], r96[7]);
    const TextureGen& tgp = texture_gen();
    if (tgp.output_valid()) {
      const auto& dv = tgp.dynamic_vertices();
      const auto& v0 = dv[0];
      const auto& vK = dv[dv.size() > 1000 ? 1000 : dv.size() - 1];
      switch_run_logf("[dk] ocean probe texgen: %u verts v0 s%.2f t%.2f rgba %u,%u,%u | v1000 "
                      "s%.2f t%.2f rgba %u,%u,%u",
                      (unsigned)dv.size(), v0.s, v0.t, (unsigned)v0.rgba[0], (unsigned)v0.rgba[1],
                      (unsigned)v0.rgba[2], vK.s, vK.t, (unsigned)vK.rgba[0],
                      (unsigned)vK.rgba[1], (unsigned)vK.rgba[2]);
    } else {
      // T99: dynamic_vertices() is zero-filled by init(), so without a
      // completed walk it would look like a decode bug. Say so explicitly.
      switch_run_logf("[dk] ocean probe texgen: NO COMPLETED WALK (output_valid 0)");
    }
    // T99: the decode-side verdict: constants range + s/t/rgba range over the
    // last completed walk. consts ~0 => chain payload problem; consts real +
    // verts ~0 => decode bug; verts vary => CPU side fine, blame the GPU draw.
    const TextureGen::WalkProbe& wp = tgp.walk_probe();
    switch_run_logf("[dk] ocean probe walk#%u: s %.2f..%.2f (%u/2112 nz) t %.2f..%.2f | rgba "
                    "max %u,%u,%u,%u | consts start %.2f off %.4f cam %.2f",
                    wp.walks, wp.s_min, wp.s_max, wp.s_nonzero, wp.t_min, wp.t_max, wp.r_max,
                    wp.g_max, wp.b_max, wp.a_max, wp.start_x, wp.offsets_x, wp.cam_nrm_x);
    GsTex0 etex(tgp.envmap_tex0_data());
    LookupFn lk = lookup();
    const unsigned eid = lk != nullptr ? lk(etex.tbp0(), false) : 0;
    switch_run_logf("[dk] ocean probe envmap: texgen tbp %u -> id %u | mid tbp %u slot %u | "
                    "near tbp %u slot %u | ocean tex slot %u",
                    (unsigned)etex.tbp0(), eid, mid_recorder().envmap_tex(),
                    s_mid_draws.envmap_slot, near_recorder().envmap_tex(),
                    s_near_draws.envmap_slot, s_mid_draws.ocean_slot);
    const CommonRecorder& mr = mid_recorder();
    if (mr.next_free_vertex() > 0) {
      const CommonRecorder::Vertex& v = mr.vertices_data()[0];
      switch_run_logf("[dk] ocean probe mid v0: xyz %.1f,%.1f,%.4f rgba %u,%u,%u,%u stq "
                      "%.2f,%.2f,%.4f fog %u | idx b0 %u b1 %u verts %u",
                      v.xyz.x(), v.xyz.y(), v.xyz.z(), (unsigned)v.rgba[0], (unsigned)v.rgba[1],
                      (unsigned)v.rgba[2], (unsigned)v.rgba[3], v.stq.x(), v.stq.y(), v.stq.z(),
                      (unsigned)v.fog, mr.next_free_index(0), mr.next_free_index(1),
                      mr.next_free_vertex());
    }
    const CommonRecorder& nr = near_recorder();
    if (nr.next_free_vertex() > 0) {
      const CommonRecorder::Vertex& v = nr.vertices_data()[0];
      switch_run_logf("[dk] ocean probe near v0: xyz %.1f,%.1f,%.4f rgba %u,%u,%u,%u stq "
                      "%.2f,%.2f,%.4f fog %u | idx b0 %u b1 %u b2 %u verts %u",
                      v.xyz.x(), v.xyz.y(), v.xyz.z(), (unsigned)v.rgba[0], (unsigned)v.rgba[1],
                      (unsigned)v.rgba[2], (unsigned)v.rgba[3], v.stq.x(), v.stq.y(), v.stq.z(),
                      (unsigned)v.fog, nr.next_free_index(0), nr.next_free_index(1),
                      nr.next_free_index(2), nr.next_free_vertex());
    }
    // FIX 113: the result-texture verdict. The body draws sample the RESULT
    // (the 8-level FIX 112 texture), not the temp - if record_mip_chain ever
    // fails silently the water goes flat/black while the temp above is healthy.
    if (s_probe_mip0.ok() && s_probe_mip3.ok()) {
      const uint8_t* q = s_probe_mip0.cpu;
      uint64_t mr = 0, mg = 0, mb = 0, ma = 0;
      for (uint32_t i = 0; i < n; i++) {
        mr += q[i * 4];
        mg += q[i * 4 + 1];
        mb += q[i * 4 + 2];
        ma += q[i * 4 + 3];
      }
      const uint8_t* q3 = s_probe_mip3.cpu;
      uint64_t r3 = 0, g3 = 0, b3 = 0, a3 = 0;
      for (uint32_t i = 0; i < 16 * 16; i++) {
        r3 += q3[i * 4];
        g3 += q3[i * 4 + 1];
        b3 += q3[i * 4 + 2];
        a3 += q3[i * 4 + 3];
      }
      switch_run_logf("[dk] ocean probe #%u MIP(result) @f%u: L0 avg %u,%u,%u,%u | L3 avg "
                      "%u,%u,%u,%u (L3 alpha fades to 0)",
                      s_probe_round + 1, s_probe_at, (unsigned)(mr / n), (unsigned)(mg / n),
                      (unsigned)(mb / n), (unsigned)(ma / n), (unsigned)(r3 / 256),
                      (unsigned)(g3 / 256), (unsigned)(b3 / 256), (unsigned)(a3 / 256));
    } else {
      switch_run_logf("[dk] ocean probe #%u MIP(result): buffer FAILED - mip chain unverified",
                      s_probe_round + 1);
    }
    // T99: give the probe buffers back to the pool and schedule the next
    // snapshot (kProbeStride texgen draws later, up to kProbeRounds total).
    dkpool::BufferPool* ppool = dkdev::buffer_pool();
    if (ppool != nullptr) {
      if (s_probe.ok()) {
        ppool->release(s_probe.id);
      }
      if (s_probe_mip0.ok()) {
        ppool->release(s_probe_mip0.id);
      }
      if (s_probe_mip3.ok()) {
        ppool->release(s_probe_mip3.id);
      }
    }
    s_probe_round++;
    s_probe_next = s_probe_draws + kProbeStride;
  }
  s_stats.frames++;
}

namespace {

// GL's exact blend/depth combinations for the ocean (see the file header).
enum OceanBlend {
  kBlendOff,       // glDisable(GL_BLEND)
  kBlendStdAlpha,  // src=SrcAlpha dst=InvSrcAlpha srcA=One dstA=Zero (near b0)
  kBlendAlphaOnly, // src=Zero dst=One srcA=One dstA=Zero (near b1)
  kBlendDstAlpha,  // src=DstAlpha dst=One srcA=One dstA=Zero (envmap)
};

void make_blend(OceanBlend kind, DkBlendState* bs) {
  dkBlendStateDefaults(bs);
  if (kind == kBlendOff) {
    return;
  }
  bs->colorBlendOp = DkBlendOp_Add;
  bs->alphaBlendOp = DkBlendOp_Add;
  bs->srcAlphaBlendFactor = DkBlendFactor_One;
  bs->dstAlphaBlendFactor = DkBlendFactor_Zero;
  switch (kind) {
    case kBlendOff:
      break;
    case kBlendStdAlpha:
      bs->srcColorBlendFactor = DkBlendFactor_SrcAlpha;
      bs->dstColorBlendFactor = DkBlendFactor_InvSrcAlpha;
      break;
    case kBlendAlphaOnly:
      bs->srcColorBlendFactor = DkBlendFactor_Zero;
      bs->dstColorBlendFactor = DkBlendFactor_One;
      break;
    case kBlendDstAlpha:
      bs->srcColorBlendFactor = DkBlendFactor_DstAlpha;
      bs->dstColorBlendFactor = DkBlendFactor_One;
      break;
  }
}

// Same mirror order as dkpipe::record_graphics_state (T35a).
// depth_always: GL flush_mid uses glDepthFunc(GL_ALWAYS) with DepthMask(TRUE) for
// BOTH buckets -- the far-walk band draws earlier in the same bucket and its
// written depth must not cull the mid plane (dk GEQUAL here was killing the whole
// mid ocean -> only the far band + gaps remained). GL flush_near uses GEQUAL.
void record_ocean_state(DkCmdBuf cb, bool depth_test, bool depth_write, OceanBlend blend,
                        bool depth_always = false) {
  DkDepthStencilState ds;
  dkDepthStencilStateDefaults(&ds);
  ds.depthTestEnable = depth_test;
  ds.depthWriteEnable = depth_write;
  ds.depthCompareOp = (!depth_test || depth_always) ? DkCompareOp_Always : DkCompareOp_Gequal;
  dkCmdBufBindDepthStencilState(cb, &ds);
  DkColorState cs;
  dkColorStateDefaults(&cs);
  if (blend != kBlendOff) {
    cs.blendEnableMask = 1;
  }
  dkCmdBufBindColorState(cb, &cs);
  DkBlendState bs;
  make_blend(blend, &bs);
  dkCmdBufBindBlendStates(cb, 0, &bs, 1);
  DkRasterizerState rs;
  dkRasterizerStateDefaults(&rs);
  rs.cullMode = DkFace_None;
  dkCmdBufBindRasterizerState(cb, &rs);
  DkColorWriteState cw;
  dkColorWriteStateDefaults(&cw);
  dkCmdBufBindColorWriteState(cb, &cw);
  DkMultisampleState ms;
  dkMultisampleStateDefaults(&ms);
  dkCmdBufBindMultisampleState(cb, &ms);
}

uint64_t common_ubo(dkring::FrameRing* ring, int bucket) {
  dkring::Alloc a = ring->alloc(kUboSize, 0x100);
  if (!a.ok()) {
    s_stats.ubo_fail++;
    return 0;
  }
  OceanCommonParams p = {};
  for (int i = 0; i < 3; i++) {
    p.fog_color[i] = s_fog_rgba[i] / 255.f;
  }
  p.fog_color[3] = s_fog_rgba[3] / 255.f;
  p.bucket = bucket;
  memcpy(a.cpu, &p, sizeof(p));
  return a.gpu;
}

// GL flush_near / flush_mid: one indexed TRIANGLE_STRIP draw per bucket.
// buckets[] pairs with the GL bucket uniform value; tex slot per draw.
// T98 debug: bucket_draws[i] counts the per-bucket draws (diagnostics).
void record_body_flush(DkCmdBuf cb, const BodyDraws& b, const int* bucket_values,
                       const uint32_t* tex_slots, int nbuckets, bool near_pass,
                       uint32_t* bucket_draws, uint32_t* draw_counter) {
  DkShader vsh = dkdev::ocean_common_vsh();
  DkShader fsh = dkdev::ocean_common_fsh();
  DkShader const* sh[] = {&vsh, &fsh};
  dkCmdBufBindShaders(cb, DkStageFlag_GraphicsMask, sh, 2);
  const DkVtxBufferState vbuf = {32, 0};
  dkCmdBufBindVtxAttribState(cb, kCommonAttribs, 4);
  dkCmdBufBindVtxBufferState(cb, &vbuf, 1);
  dkCmdBufBindVtxBuffer(cb, 0, b.vert_gpu, b.vert_bytes);
  dkCmdBufSetPrimitiveRestart(cb, true, UINT32_MAX);
  dkring::FrameRing* ring = dkdev::ubo_ring();
  for (int i = 0; i < nbuckets; i++) {
    if (b.idx_count[i] == 0 || tex_slots[i] == UINT32_MAX) {
      if (tex_slots[i] == UINT32_MAX && b.idx_count[i] != 0) {
        s_stats.no_tex++;
      }
      continue;
    }
    uint64_t ubo = common_ubo(ring, bucket_values[i]);
    if (ubo == 0) {
      break;
    }
    // near: depth GEQUAL, no write; mid: depth ALWAYS + write (GL flush_*).
    if (near_pass) {
      // b0 = StdAlpha, b1 = alpha-only write, b2 = envmap DST_ALPHA/ONE.
      const OceanBlend bl =
          i == 0 ? kBlendStdAlpha : (i == 1 ? kBlendAlphaOnly : kBlendDstAlpha);
      record_ocean_state(cb, true, false, bl);
    } else {
      // FIX 114 (AI-assisted): depth_always=true, like GL flush_mid's
      // glDepthFunc(GL_ALWAYS). The far band recorded just above writes depth at
      // the same / farther z, so GEQUAL culled the whole mid plane (black gaps in
      // the water, broken horizon band, near-envmap DstAlpha blending against
      // sky alpha instead of the mid plane's own alpha write).
      record_ocean_state(cb, true, true, i == 0 ? kBlendOff : kBlendDstAlpha, true);
    }
    dkCmdBufBindUniformBuffer(cb, DkStage_Vertex, 6, ubo, kUboSize);
    dkCmdBufBindUniformBuffer(cb, DkStage_Fragment, 7, ubo, kUboSize);
    uint8_t samp = 0;
    // FIX 112: GL's mid pass samples LINEAR_MIPMAP_LINEAR on both buckets, and
    // the near envmap bucket inherits mip filtering from the mid flush having
    // touched the same texture object earlier in the frame. Near b0/b1 stay
    // plain LINEAR (GL flush_near sets it explicitly).
    const bool mip = near_pass ? (i == 2) : true;
    if (!dkpipe::find_sampler(false, false, true, mip, &samp)) {  // REPEAT + LINEAR (+mip)
      s_stats.misses++;
      continue;
    }
    dkCmdBufBindTexture(cb, DkStage_Fragment, 0, dkMakeTextureHandle(tex_slots[i], samp));
    dkCmdBufBindIdxBuffer(cb, DkIdxFormat_Uint32, b.idx_gpu[i]);
    dkCmdBufDrawIndexed(cb, DkPrimitive_TriangleStrip, b.idx_count[i], 1, 0, 0, 0);
    bucket_draws[i]++;
    (*draw_counter)++;
  }
}

// FIX 112 (AI-assisted): GL OceanTexture::make_texture_with_mipmaps. GL renders
// the texgen into a temp texture, then redraws every level of the result
// (sampled from temp, LINEAR) with alpha *= max(0, 1 - 0.51*i) - the distance
// fade that turns the envmap reflection (the DST_ALPHA blends) off from mip 2
// down. The port had a single-mip result (T95 limitation), so the envmap never
// faded (thin blue line at the horizon) and the distant water minified the
// 128x128 texture unfiltered (black spots / sparkle). Eight tiny quads.
void record_mip_chain(DkCmdBuf cb) {
  if (s_mip_quad_gpu == 0 || s_ocean_tex_id == 0 || s_texgen_rt_id == 0 ||
      !dkdev::ocean_mip_shaders_loaded()) {
    return;
  }
  const uint32_t temp_slot = dktexgpu::slot_of(s_texgen_rt_id);
  if (temp_slot == UINT32_MAX) {
    return;
  }
  uint8_t samp = 0;
  if (!dkpipe::find_sampler(false, false, true, false, &samp)) {  // REPEAT + LINEAR
    s_stats.misses++;
    return;
  }
  const DkImage* img = (const DkImage*)dktexgpu::image_of(s_ocean_tex_id);
  if (img == nullptr) {
    return;
  }
  DkShader vsh = dkdev::ocean_mip_vsh();
  DkShader fsh = dkdev::ocean_mip_fsh();
  DkShader const* sh[] = {&vsh, &fsh};
  dkCmdBufBindShaders(cb, DkStageFlag_GraphicsMask, sh, 2);
  const DkVtxAttribState attribs[] = {
      {0, 0, 0, DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},  // pos f2
      {0, 0, 8, DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},  // st f2
  };
  const DkVtxBufferState vbuf = {16, 0};
  dkCmdBufBindVtxAttribState(cb, attribs, 2);
  dkCmdBufBindVtxBufferState(cb, &vbuf, 1);
  dkCmdBufBindVtxBuffer(cb, 0, s_mip_quad_gpu, 4 * 16);
  record_ocean_state(cb, false, false, kBlendOff);  // GL: no depth, no blend
  dkCmdBufBindTexture(cb, DkStage_Fragment, 0, dkMakeTextureHandle(temp_slot, samp));
  dkring::FrameRing* ring = dkdev::ubo_ring();
  static bool s_logged = false;
  for (uint32_t i = 0; i < kOceanTexMips; i++) {
    dkring::Alloc a = ring->alloc(kUboSize, 0x100);
    if (!a.ok()) {
      s_stats.ubo_fail++;
      return;  // later levels keep last frame's content for one frame
    }
    OceanMipParams p = {};
    p.params[0] = std::max(0.0f, 1.0f - 0.51f * (float)i);  // GL's exact curve
    memcpy(a.cpu, &p, sizeof(p));
    DkImageView view;
    dkImageViewDefaults(&view, img);
    view.mipLevelOffset = (uint8_t)i;
    view.mipLevelCount = 1;
    dkCmdBufBindRenderTarget(cb, &view, nullptr);
    const uint32_t lw = std::max<uint32_t>(1u, kOceanTexSize >> i);
    DkViewport vp = {0.0f, 0.0f, (float)lw, (float)lw, 0.0f, 1.0f};
    dkCmdBufSetViewports(cb, 0, &vp, 1);
    DkScissor sc = {0, 0, lw, lw};
    dkCmdBufSetScissors(cb, 0, &sc, 1);
    dkCmdBufBindUniformBuffer(cb, DkStage_Fragment, 7, a.gpu, kUboSize);
    dkCmdBufDraw(cb, DkPrimitive_TriangleStrip, 4, 1, 0, 0);
  }
  s_stats.tex_mip_draws++;
  if (!s_logged) {
    s_logged = true;
    switch_run_logf("[dk] ocean mip chain running (%u levels)", kOceanTexMips);
  }
}

// GL OceanTexture::flush: draw the VU-computed wave mesh into the 128x128
// ocean-texture render target, sampling the envmap texture (T82 eye pattern).
void record_texture_gen(DkCmdBuf cb) {
  const TextureGen& tg = texture_gen();
  if (!s_texdyn_valid || s_static_pos_gpu == 0 || s_texgen_rt_id == 0 ||
      s_ocean_tex_id == 0 || !dkdev::ocean_tex_shaders_loaded()) {
    return;
  }
  GsTex0 tex0(tg.envmap_tex0_data());
  LookupFn lk = lookup();
  uint32_t envmap_id = lk != nullptr ? lk(tex0.tbp0(), false) : 0;
  uint32_t envmap_slot = slot_of_or_max(envmap_id);
  if (envmap_slot == UINT32_MAX) {
    s_stats.no_tex++;  // GL falls back to the placeholder; we keep last frame
    return;
  }
  uint8_t samp = 0;
  // FIX 113: GL flush binds the envmap with LINEAR_MIPMAP_LINEAR here
  // (OceanTexture_PC.cpp flush). A single-mip envmap image clamps to L0, so
  // this changes nothing there and matches GL exactly when it has mips.
  if (!dkpipe::find_sampler(false, false, true, true, &samp)) {
    s_stats.misses++;
    return;
  }
  const DkImage* img = (const DkImage*)dktexgpu::image_of(s_texgen_rt_id);  // FIX 112: temp
  if (img == nullptr) {
    return;
  }
  DkImageView view;
  dkImageViewDefaults(&view, img);
  dkCmdBufBindRenderTarget(cb, &view, nullptr);
  DkViewport vp = {0.0f, 0.0f, (float)kOceanTexSize, (float)kOceanTexSize, 0.0f, 1.0f};
  dkCmdBufSetViewports(cb, 0, &vp, 1);
  DkScissor sc = {0, 0, kOceanTexSize, kOceanTexSize};
  dkCmdBufSetScissors(cb, 0, &sc, 1);

  DkShader vsh = dkdev::ocean_tex_vsh();
  DkShader fsh = dkdev::ocean_tex_fsh();
  DkShader const* sh[] = {&vsh, &fsh};
  dkCmdBufBindShaders(cb, DkStageFlag_GraphicsMask, sh, 2);
  const DkVtxBufferState vbufs[2] = {{8, 0}, {16, 0}};
  dkCmdBufBindVtxAttribState(cb, kTexAttribs, 3);
  dkCmdBufBindVtxBufferState(cb, vbufs, 2);
  dkCmdBufBindVtxBuffer(cb, 0, s_static_pos_gpu, 2112 * 8);
  dkCmdBufBindVtxBuffer(cb, 1, s_texdyn_gpu, s_texdyn_bytes);
  dkCmdBufSetPrimitiveRestart(cb, true, UINT32_MAX);
  record_ocean_state(cb, false, false, kBlendOff);  // GL: no depth, no blend
  dkCmdBufBindTexture(cb, DkStage_Fragment, 0, dkMakeTextureHandle(envmap_slot, samp));
  dkCmdBufBindIdxBuffer(cb, DkIdxFormat_Uint32, s_static_idx_gpu);
  dkCmdBufDrawIndexed(cb, DkPrimitive_TriangleStrip, s_static_idx_count, 1, 0, 0, 0);
  s_stats.tex_gen_draws++;

  // The mip pass samples the temp right after: finish the fragments and drop
  // stale texture-cache lines (same barrier as the eye recorder).
  dkCmdBufBarrier(cb, DkBarrier_Fragments, DkInvalidateFlags_Image | DkInvalidateFlags_L2Cache);
  // FIX 112: temp -> all 8 result levels (GL make_texture_with_mipmaps).
  record_mip_chain(cb);
  // The mid body draws sample the result right after: barrier again.
  dkCmdBufBarrier(cb, DkBarrier_Fragments, DkInvalidateFlags_Image | DkInvalidateFlags_L2Cache);
  dkdev::rebind_frame_target();
}

}  // namespace

// T95: bucket 4 (GL OceanMidAndFar order): texture gen -> far draws -> mid flush.
void record_mid_far(int fb_w, int fb_h) {
  if (!dkdev::ocean_common_shaders_loaded()) {
    return;
  }
  DkCmdBuf cb = dkdev::cmd_handle();
  const DkGpuAddr data = dkMemBlockGetGpuAddr(dkdev::data_mem());
  // Descriptor sets first: the texture-gen pass binds textures too.
  dkCmdBufBindImageDescriptorSet(cb, data + dkdev::desc_off() + 0x100, kTexImages);
  dkCmdBufBindSamplerDescriptorSet(cb, data + dkdev::sampler_set_off(),
                                   dkdev::sampler_set_count());
  record_texture_gen(cb);
  // T99 debug probe: copy the ocean RT during real gameplay - armed only after
  // the texgen GPU draw has actually run (T98 armed on build count and sampled
  // an idle ocean), repeating every kProbeStride draws up to kProbeRounds.
  if (!s_probe_armed && s_probe_round < kProbeRounds && s_ocean_tex_id != 0 &&
      s_stats.tex_gen_draws >= s_probe_next) {
    dkpool::BufferPool* pool = dkdev::buffer_pool();
    const DkImage* pimg = (const DkImage*)dktexgpu::image_of(s_texgen_rt_id);  // FIX 112: temp = texgen output
    if (pool != nullptr && pimg != nullptr) {
      s_probe = pool->acquire(kOceanTexSize * kOceanTexSize * 4);
      if (s_probe.ok()) {
        DkImageView pv;
        dkImageViewDefaults(&pv, pimg);
        const DkCopyBuf pcbuf = {s_probe.gpu, 0, 0};
        const DkImageRect prc = {0, 0, 0, kOceanTexSize, kOceanTexSize, 1};
        dkCmdBufCopyImageToBuffer(cb, &pv, &prc, &pcbuf, 0);
        // FIX 113: result texture L0 (full) + L3 (16x16, alpha faded to 0 by
        // GL's curve). Copies are read-only and run after record_mip_chain.
        const DkImage* rimg = (const DkImage*)dktexgpu::image_of(s_ocean_tex_id);
        if (rimg != nullptr) {
          s_probe_mip0 = pool->acquire(kOceanTexSize * kOceanTexSize * 4);
          s_probe_mip3 = pool->acquire(16 * 16 * 4);
          if (s_probe_mip0.ok() && s_probe_mip3.ok()) {
            DkImageView v0;
            dkImageViewDefaults(&v0, rimg);
            v0.mipLevelOffset = 0;
            v0.mipLevelCount = 1;
            const DkCopyBuf c0 = {s_probe_mip0.gpu, 0, 0};
            const DkImageRect r0 = {0, 0, 0, kOceanTexSize, kOceanTexSize, 1};
            dkCmdBufCopyImageToBuffer(cb, &v0, &r0, &c0, 0);
            DkImageView v3;
            dkImageViewDefaults(&v3, rimg);
            v3.mipLevelOffset = 3;
            v3.mipLevelCount = 1;
            const DkCopyBuf c3 = {s_probe_mip3.gpu, 0, 0};
            const DkImageRect r3 = {0, 0, 0, 16, 16, 1};
            dkCmdBufCopyImageToBuffer(cb, &v3, &r3, &c3, 0);
          }
        }
        s_probe_at = s_build;
        s_probe_draws = s_stats.tex_gen_draws;
        s_probe_armed = true;
      }
    }
  }
  dkdirect::record_ocean_far_world(fb_w, fb_h);  // GL: far before mid
  if (!s_mid_draws.valid) {
    return;
  }
  DkViewport vp = {0.0f, 0.0f, (float)fb_w, (float)fb_h, 0.0f, 1.0f};
  dkCmdBufSetViewports(cb, 0, &vp, 1);
  const int buckets[2] = {3, 4};
  const uint32_t slots[2] = {s_mid_draws.ocean_slot, s_mid_draws.envmap_slot};
  record_body_flush(cb, s_mid_draws, buckets, slots, 2, false, s_stats.mid_bucket_draws,
                    &s_stats.mid_draws);
}

// T95: bucket 63 (GL OceanNear flush_near).
void record_near(int fb_w, int fb_h) {
  if (!s_near_draws.valid || !dkdev::ocean_common_shaders_loaded()) {
    return;
  }
  DkCmdBuf cb = dkdev::cmd_handle();
  const DkGpuAddr data = dkMemBlockGetGpuAddr(dkdev::data_mem());
  DkViewport vp = {0.0f, 0.0f, (float)fb_w, (float)fb_h, 0.0f, 1.0f};
  dkCmdBufSetViewports(cb, 0, &vp, 1);
  dkCmdBufBindImageDescriptorSet(cb, data + dkdev::desc_off() + 0x100, kTexImages);
  dkCmdBufBindSamplerDescriptorSet(cb, data + dkdev::sampler_set_off(),
                                   dkdev::sampler_set_count());
  const int buckets[3] = {0, 1, 2};
  const uint32_t slots[3] = {s_near_draws.ocean_slot, s_near_draws.ocean_slot,
                            s_near_draws.envmap_slot};
  record_body_flush(cb, s_near_draws, buckets, slots, 3, true, s_stats.near_bucket_draws,
                    &s_stats.near_draws);
}

const FlushStats& flush_stats() {
  return s_stats;
}

}  // namespace dkocean
#endif  // __SWITCH__
