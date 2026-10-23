/*
 * dk_ocean_common.cpp - deko3d ocean common recorder (T93, AI-assisted)
 *
 * Decode half of CommonOceanRenderer, copied verbatim from
 * game/graphics/opengl_renderer/ocean/CommonOceanRenderer.cpp (lines 79-279,
 * 354-457). The only changes are: CommonRecorder:: renamed to
 * CommonRecorder::, wrapped in namespace dkocean, and the constructor is
 * the minimal (empty) replacement of the GL one - the GL ctor/dtor and
 * flush_near/flush_mid stay in the GL file and are NOT ported here.
 */

#include "game/graphics/deko3d_renderer/dk_ocean_common.h"

#include "common/dma/gs.h"
#include "common/log/log.h"
#include "common/util/Assert.h"
#include "game/graphics/opengl_renderer/AdgifHandler.h"

#include <algorithm>
#include <cstring>

// Same pattern as dk_ocean_flush.cpp: run_log.h is Switch-only and cannot be
// included here, but the console log needs switch_run_logf (log format: [dk]
// prefix, one line).
#if defined(__SWITCH__)
void switch_run_logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
#endif

constexpr int OCEAN_TEX_TBP_JAK1 = 8160;  // todo
constexpr int OCEAN_TEX_TBP_JAK2 = 672;

namespace dkocean {

// T96 (AI-assisted): the T93 build shipped this constructor EMPTY, unlike the
// GL CommonOceanRenderer one (and the T93 spec) which resizes the decode
// buffers. The vectors stayed empty (null data pointers) and the first console
// ocean kick crashed: gk_fatal 2026-10-23 jak1, data abort write far=0x0 at
// handle_near_vertex_gif_data_strip+0x2c, called from kick_from_mid+0xec.
// GL's fixed 4096*10 is ~3.5 MB across the near+mid recorders (Switch heap
// headroom is ~4 MB), so start at kInitialCapacity and grow on demand.
CommonRecorder::CommonRecorder() {
  m_vertices.resize(kInitialCapacity);
  for (auto& buf : m_indices) {
    buf.resize(kInitialCapacity);
  }
}

bool CommonRecorder::ensure_capacity(u32 extra_verts, u32 extra_indices) {
  const u32 want_v = m_next_free_vertex + extra_verts;
  const u32 want_i = m_next_free_index[m_current_bucket] + extra_indices;
  if (want_v <= (u32)m_vertices.size() &&
      want_i <= (u32)m_indices[m_current_bucket].size()) {
    return true;
  }
  if (want_v > kMaxCapacity || want_i > kMaxCapacity) {
    return false;
  }
  u32 cap = std::max<u32>({(u32)m_vertices.size(), (u32)m_indices[m_current_bucket].size(), 1u});
  while (cap < want_v || cap < want_i) {
    cap *= 2;
  }
  cap = std::min(cap, kMaxCapacity);
  m_vertices.resize(cap);
  for (auto& buf : m_indices) {
    buf.resize(cap);
  }
#if defined(__SWITCH__)
  switch_run_logf("[dk] ocean decode buffers grew to %u (v=%u+%u i=%u+%u)", cap, m_next_free_vertex,
                  extra_verts, m_next_free_index[m_current_bucket], extra_indices);
#else
  lg::debug("ocean decode buffers grew to {}", cap);
#endif
  return true;
}

void CommonRecorder::init_for_near() {
  m_next_free_vertex = 0;
  for (auto& x : m_next_free_index) {
    x = 0;
  }
}

void CommonRecorder::kick_from_near(const u8* data) {
  bool eop = false;

  u32 offset = 0;
  while (!eop) {
    GifTag tag(data + offset);
    offset += 16;

    if (tag.nreg() == 3) {
      ASSERT(tag.pre());
      if (GsPrim(tag.prim()).kind() == GsPrim::Kind::TRI_STRIP) {
        handle_near_vertex_gif_data_strip(data, offset, tag.nloop());
      } else {
        handle_near_vertex_gif_data_fan(data, offset, tag.nloop());
      }
      offset += 16 * 3 * tag.nloop();
    } else if (tag.nreg() == 1) {
      handle_near_adgif(data, offset, tag.nloop());
      offset += 16 * 1 * tag.nloop();
    } else {
      ASSERT(false);
    }

    eop = tag.eop();
  }
}

void CommonRecorder::handle_near_vertex_gif_data_strip(const u8* data, u32 offset, u32 loop) {
  // T96 (AI-assisted): this kick writes up to `loop` vertices and 1 + 3*loop
  // indices (3 per adc vertex). If it cannot fit even at GL's design max,
  // drop it - the caller advances its own offset, so the chain stays in sync.
  if (!ensure_capacity(loop, 3 * loop + 1)) {
    return;
  }
  m_indices[m_current_bucket][m_next_free_index[m_current_bucket]++] = UINT32_MAX;
  bool reset_last = false;
  for (u32 i = 0; i < loop; i++) {
    auto& dest_vert = m_vertices[m_next_free_vertex++];

    // stq
    memcpy(dest_vert.stq.data(), data + offset, 12);
    offset += 16;

    // rgba
    dest_vert.rgba[0] = data[offset];
    dest_vert.rgba[1] = data[offset + 4];
    dest_vert.rgba[2] = data[offset + 8];
    dest_vert.rgba[3] = data[offset + 12];
    offset += 16;

    // xyz
    u32 x = 0, y = 0;
    memcpy(&x, data + offset, 4);
    memcpy(&y, data + offset + 4, 4);

    u64 upper;
    memcpy(&upper, data + offset + 8, 8);
    u32 z = (upper >> 4) & 0xffffff;
    offset += 16;

    dest_vert.xyz[0] = (float)(x << 16) / (float)UINT32_MAX;
    dest_vert.xyz[1] = (float)(y << 16) / (float)UINT32_MAX;
    dest_vert.xyz[2] = (float)(z << 8) / (float)UINT32_MAX;

    u8 f = (upper >> 36);
    dest_vert.fog = f;

    auto vidx = m_next_free_vertex - 1;
    bool adc = upper & (1ull << 47);
    if (!adc) {
      m_indices[m_current_bucket][m_next_free_index[m_current_bucket]++] = vidx;
      reset_last = false;
    } else {
      if (reset_last) {
        m_next_free_index[m_current_bucket] -= 3;
      }
      m_indices[m_current_bucket][m_next_free_index[m_current_bucket]++] = UINT32_MAX;
      m_indices[m_current_bucket][m_next_free_index[m_current_bucket]++] = vidx - 1;
      m_indices[m_current_bucket][m_next_free_index[m_current_bucket]++] = vidx;
      reset_last = true;
    }
  }
}

void CommonRecorder::handle_near_vertex_gif_data_fan(const u8* data, u32 offset, u32 loop) {
  // T96 (AI-assisted): fan emulation writes up to 4 indices per vertex (from
  // the second one on). Drop the kick if it cannot fit at GL's design max.
  if (!ensure_capacity(loop, 4 * loop)) {
    return;
  }
  u32 ind_of_fan_start = UINT32_MAX;
  bool fan_running = false;
  // :regs0 (gif-reg-id st) :regs1 (gif-reg-id rgbaq) :regs2 (gif-reg-id xyzf2)
  for (u32 i = 0; i < loop; i++) {
    auto& dest_vert = m_vertices[m_next_free_vertex++];

    // stq
    memcpy(dest_vert.stq.data(), data + offset, 12);
    offset += 16;

    // rgba
    dest_vert.rgba[0] = data[offset];
    dest_vert.rgba[1] = data[offset + 4];
    dest_vert.rgba[2] = data[offset + 8];
    dest_vert.rgba[3] = data[offset + 12];
    offset += 16;

    // xyz
    u32 x = 0, y = 0;
    memcpy(&x, data + offset, 4);
    memcpy(&y, data + offset + 4, 4);

    u64 upper;
    memcpy(&upper, data + offset + 8, 8);
    u32 z = (upper >> 4) & 0xffffff;
    offset += 16;

    dest_vert.xyz[0] = (float)(x << 16) / (float)UINT32_MAX;
    dest_vert.xyz[1] = (float)(y << 16) / (float)UINT32_MAX;
    dest_vert.xyz[2] = (float)(z << 8) / (float)UINT32_MAX;

    u8 f = (upper >> 36);
    dest_vert.fog = f;

    auto vidx = m_next_free_vertex - 1;

    if (ind_of_fan_start == UINT32_MAX) {
      ind_of_fan_start = vidx;
    } else {
      if (fan_running) {
        // hack to draw fans with strips. this isn't efficient, but fans happen extremely rarely
        // (you basically have to put the camera intersecting the ocean and looking fwd)
        m_indices[m_current_bucket][m_next_free_index[m_current_bucket]++] = UINT32_MAX;
        m_indices[m_current_bucket][m_next_free_index[m_current_bucket]++] = vidx;
        m_indices[m_current_bucket][m_next_free_index[m_current_bucket]++] = vidx - 1;
        m_indices[m_current_bucket][m_next_free_index[m_current_bucket]++] = ind_of_fan_start;
      } else {
        fan_running = true;
      }
    }
  }
}

void CommonRecorder::handle_near_adgif(const u8* data, u32 offset, u32 count) {
  u32 most_recent_tbp = 0;

  for (u32 i = 0; i < count; i++) {
    u64 value;
    GsRegisterAddress addr;
    memcpy(&value, data + offset + 16 * i, sizeof(u64));
    memcpy(&addr, data + offset + 16 * i + 8, sizeof(GsRegisterAddress));
    switch (addr) {
      case GsRegisterAddress::MIPTBP1_1:
        // ignore this, it's just mipmapping settings
        break;
      case GsRegisterAddress::TEX1_1: {
        GsTex1 reg(value);
        ASSERT(reg.mmag());
      } break;
      case GsRegisterAddress::CLAMP_1: {
        bool s = value & 0b001;
        bool t = value & 0b100;
        ASSERT(s == t);
        if (s) {
          m_current_bucket = VertexBucket::ENV_MAP;
        }
      } break;
      case GsRegisterAddress::TEX0_1: {
        GsTex0 reg(value);
        ASSERT(reg.tfx() == GsTex0::TextureFunction::MODULATE);
        if (!reg.tcc()) {
          m_current_bucket = VertexBucket::RGB_TEXTURE;
        }
        most_recent_tbp = reg.tbp0();
      } break;
      case GsRegisterAddress::ALPHA_1: {
        GsAlpha reg(value);
        if (reg.a_mode() == GsAlpha::BlendMode::SOURCE &&
            reg.b_mode() == GsAlpha::BlendMode::ZERO_OR_FIXED &&
            reg.c_mode() == GsAlpha::BlendMode::DEST && reg.d_mode() == GsAlpha::BlendMode::DEST) {
          m_current_bucket = VertexBucket::ENV_MAP;
        }
      } break;

      case GsRegisterAddress::FRAME_1: {
        u32 mask = value >> 32;
        if (mask) {
          m_current_bucket = VertexBucket::ALPHA;
        }
      } break;

      default:
        lg::debug("reg: {}", register_address_name(addr));
        break;
    }
  }

  if (m_current_bucket == VertexBucket::ENV_MAP) {
    m_envmap_tex = most_recent_tbp;
  }

  // GL: `m_vertices.size() - 128 < m_next_free_vertex` + ASSERT(false) ("add
  // more vertices"). size()-128 underflows when size() < 128, and ASSERT is
  // fatal on console. ensure_capacity already guarantees room up to GL's
  // 4096*10, so landing here means a frame beyond GL's design max too: log
  // (throttled) and keep going instead of killing the game.
  if (m_next_free_vertex + 128 > (u32)m_vertices.size()) {
    static u32 s_warn = 0;
    if ((s_warn++ & 0x3f) == 0) {
#if defined(__SWITCH__)
      switch_run_logf("[dk] ocean near adgif headroom low: %u/%u verts", m_next_free_vertex,
                      (u32)m_vertices.size());
#else
      lg::debug("ocean near adgif headroom low: {}/{} verts", m_next_free_vertex,
                m_vertices.size());
#endif
    }
  }
}
void CommonRecorder::kick_from_mid(const u8* data) {
  bool eop = false;

  u32 offset = 0;
  while (!eop) {
    GifTag tag(data + offset);
    offset += 16;

    // unpack registers.
    // faster to do it once outside of the nloop loop.
    GifTag::RegisterDescriptor reg_desc[16];
    u32 nreg = tag.nreg();
    for (u32 i = 0; i < nreg; i++) {
      reg_desc[i] = tag.reg(i);
    }

    auto format = tag.flg();
    if (format == GifTag::Format::PACKED) {
      if (tag.nreg() == 1) {
        ASSERT(!tag.pre());
        ASSERT(tag.nloop() == 5);
        handle_mid_adgif(data, offset);
        offset += 5 * 16;
      } else {
        ASSERT(tag.nreg() == 3);
        ASSERT(tag.pre());
        m_current_bucket = GsPrim(tag.prim()).abe() ? 1 : 0;

        int count = tag.nloop();
        if (GsPrim(tag.prim()).kind() == GsPrim::Kind::TRI_STRIP) {
          handle_near_vertex_gif_data_strip(data, offset, tag.nloop());
        } else {
          handle_near_vertex_gif_data_fan(data, offset, tag.nloop());
        }
        offset += 3 * 16 * count;
        // todo handle.
      }
    } else {
      ASSERT(false);  // format not packed or reglist.
    }

    eop = tag.eop();
  }
}

void CommonRecorder::handle_mid_adgif(const u8* data, u32 offset) {
  u32 most_recent_tbp = 0;

  for (u32 i = 0; i < 5; i++) {
    u64 value;
    GsRegisterAddress addr;
    memcpy(&value, data + offset + 16 * i, sizeof(u64));
    memcpy(&addr, data + offset + 16 * i + 8, sizeof(GsRegisterAddress));
    switch (addr) {
      case GsRegisterAddress::MIPTBP1_1:
      case GsRegisterAddress::MIPTBP2_1:
        // ignore this, it's just mipmapping settings
        break;
      case GsRegisterAddress::TEX1_1: {
        GsTex1 reg(value);
        ASSERT(reg.mmag());
      } break;
      case GsRegisterAddress::CLAMP_1: {
        bool s = value & 0b001;
        bool t = value & 0b100;
        ASSERT(s == t);
      } break;
      case GsRegisterAddress::TEX0_1: {
        GsTex0 reg(value);
        ASSERT(reg.tfx() == GsTex0::TextureFunction::MODULATE);
        most_recent_tbp = reg.tbp0();
      } break;
      case GsRegisterAddress::ALPHA_1: {
      } break;

      default:
        lg::debug("reg: {}", register_address_name(addr));
        break;
    }
  }

  if (most_recent_tbp != OCEAN_TEX_TBP_JAK2) {
    m_envmap_tex = most_recent_tbp;
  }

  // Same T96 change as handle_near_adgif: no size_t underflow, no fatal
  // ASSERT on console - log (throttled) and keep going.
  if (m_next_free_vertex + 128 > (u32)m_vertices.size()) {
    static u32 s_warn = 0;
    if ((s_warn++ & 0x3f) == 0) {
#if defined(__SWITCH__)
      switch_run_logf("[dk] ocean mid adgif headroom low: %u/%u verts", m_next_free_vertex,
                      (u32)m_vertices.size());
#else
      lg::debug("ocean mid adgif headroom low: {}/{} verts", m_next_free_vertex,
                m_vertices.size());
#endif
    }
  }
}

void CommonRecorder::init_for_mid() {
  m_next_free_vertex = 0;
  for (auto& x : m_next_free_index) {
    x = 0;
  }
}

void reverse_indices(u32* indices, u32 count) {
  if (count) {
    for (u32 a = 0, b = count - 1; a < b; a++, b--) {
      std::swap(indices[a], indices[b]);
    }
  }
}

}  // namespace dkocean
