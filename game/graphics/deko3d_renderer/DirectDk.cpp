#include "game/graphics/deko3d_renderer/DirectDk.h"

// T80 (AI-assisted): deko3d DirectRenderer CPU half. Port of GL opengl_renderer/DirectRenderer.cpp
// (render -> render_vif -> render_gif -> handle_* -> flush_pending) for jak1 buckets 67-69.
// Every place GL calls flush_pending() closes a Batch here, with the state GL would have bound.
// DK_HOST_REPLAY: test/test_dk_direct_replay.cpp compiles this file on the Mac.
#if defined(__SWITCH__) || defined(DK_HOST_REPLAY)
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "common/dma/dma_chain_read.h"
#include "common/dma/gs.h"

#include "game/graphics/deko3d_renderer/dk_buffer_pool.h"
#include "game/graphics/pipelines/dk_device.h"
#include "game/switch/run_log.h"

namespace dkdirect {
namespace {

constexpr uint32_t kMaxVerts = 200000;  // whole frame, all three buckets
constexpr int kSlots = 3;               // ring slots, same reason as MercDk / SpriteDk

std::vector<Vertex> s_verts;
std::vector<Batch> s_batches;
CaptureStats s_stats;
FrameDraws s_frame;
LookupFn s_lookup = nullptr;

// ---- GL DirectRenderer member state (reset per bucket by reset_state) ----
struct TestState {
  GsTest reg;
  bool ate = false;
  GsTest::AlphaTest atst = GsTest::AlphaTest::NOTEQUAL;
  uint8_t aref = 0;
  GsTest::AlphaFail afail = GsTest::AlphaFail::KEEP;
  GsTest::ZTest ztst = GsTest::ZTest::GEQUAL;
  bool depth_writes = true;
} s_test;

struct BlendState {
  GsAlpha reg;
  GsAlpha::BlendMode a = GsAlpha::BlendMode::SOURCE, b = GsAlpha::BlendMode::DEST,
                     c = GsAlpha::BlendMode::SOURCE, d = GsAlpha::BlendMode::DEST;
  bool abe = false;
  uint8_t fix = 0;
} s_blend;

struct PrimState {
  GsPrim reg;
  bool tme = false, fge = false, fst = false;
  uint32_t ta0 = 0;
} s_prim;

struct TexState {
  GsTex0 reg;
  uint32_t tbp = 0;
  bool mt4hh = false, tcc = false, decal = false, filt = true;
  uint64_t clamp_reg = 0b101;
  bool clamp_s = true, clamp_t = true;
};
TexState s_tex_reg;      // m_tex_state_from_reg
TexState s_tex_buf;      // m_buffered_tex_state[0]
bool s_tex_buf_used = false;
bool s_tex_current = false;  // m_current_tex_state_idx != -1

struct Build {
  GsPrim::Kind kind = GsPrim::Kind::PRIM_7;
  uint8_t rgba[4] = {0, 0, 0, 0};
  float st[2] = {0, 0};
  float Q = 1.f;
  uint8_t b_rgba[3][4];
  uint32_t b_vert[3][4];
  float b_stq[3][3];
  int idx = 0;
  int strip_startup = 0;
} s_pb;

// static in GL (shared across buckets and frames); the enable flag is per bucket renderer and
// is never cleared once set.
struct {
  uint16_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
} s_scissor;
bool s_scissor_enable[3] = {false, false, false};
int s_bucket_idx = 0;
float s_double_aref = 0.f;   // m_double_draw_aref (only updated by an alpha test)
uint32_t s_batch_first = 0;  // first vertex of the open batch

void reset_state() {
  s_test = TestState();
  s_blend = BlendState();
  s_prim = PrimState();
  s_tex_reg = TexState();
  s_tex_buf = TexState();
  s_tex_buf_used = false;
  s_tex_current = false;
  s_pb = Build();
}

// GL flush_pending: draw what is pending with the state bound right now.
void flush() {
  const uint32_t n = (uint32_t)s_verts.size() - s_batch_first;
  const bool tex_used = s_tex_buf_used;
  s_tex_buf_used = false;
  s_tex_current = false;
  if (n == 0) {
    return;
  }
  Batch bt;
  bt.first = s_batch_first;
  bt.count = n;
  bt.bucket = kJak1Buckets[s_bucket_idx];
  bt.textured = s_prim.tme;

  // update_gl_prim
  if (bt.textured) {
    if (s_test.ate) {
      switch (s_test.atst) {
        case GsTest::AlphaTest::GEQUAL:
          bt.alpha_min = s_test.aref / 128.f;
          s_double_aref = bt.alpha_min;
          break;
        case GsTest::AlphaTest::GREATER:
          bt.alpha_min = (1 + s_test.aref) / 128.f;
          s_double_aref = bt.alpha_min;
          bt.greater = true;
          break;
        case GsTest::AlphaTest::ALWAYS:
        case GsTest::AlphaTest::NEVER:
          break;
        default:
          s_stats.bad++;  // GL asserts
          break;
      }
    }
    bt.ta0 = s_prim.ta0 / 255.f;
  }

  // update_gl_test
  switch (s_test.ztst) {
    case GsTest::ZTest::NEVER:
      bt.depth_op = 3;
      break;
    case GsTest::ZTest::ALWAYS:
      bt.depth_op = 0;
      break;
    case GsTest::ZTest::GEQUAL:
      bt.depth_op = 1;
      break;
    default:
      bt.depth_op = 2;
      break;
  }
  bt.depth_test = true;
  const bool alpha_trick = s_test.ate && s_test.atst == GsTest::AlphaTest::NEVER &&
                           s_test.afail == GsTest::AlphaFail::FB_ONLY;
  bt.depth_write = s_test.depth_writes && !alpha_trick;
  bt.double_draw = bt.textured && (s_test.afail == GsTest::AlphaFail::FB_ONLY ||
                                   s_test.afail == GsTest::AlphaFail::RGB_ONLY);
  bt.double_aref = s_double_aref;

  // update_gl_blend -> dkpipe::Blend (0 Off, 1 StdAlpha, 2 PremulAdd, 5 SrcAlphaSub,
  // 6 DstAlphaAdd). (Cs-Cd)*FIX+Cd = StdAlpha with the shader writing FIX/127 as alpha.
  if (s_blend.abe) {
    using B = GsAlpha::BlendMode;
    const auto a = s_blend.a, b = s_blend.b, c = s_blend.c, d = s_blend.d;
    if (a == B::SOURCE && b == B::DEST && c == B::SOURCE && d == B::DEST) {
      bt.blend = 1;
    } else if (a == B::SOURCE && b == B::ZERO_OR_FIXED && c == B::SOURCE && d == B::DEST) {
      bt.blend = 2;
    } else if (a == B::ZERO_OR_FIXED && b == B::SOURCE && c == B::SOURCE && d == B::DEST) {
      bt.blend = 5;
    } else if (a == B::SOURCE && b == B::DEST && c == B::ZERO_OR_FIXED && d == B::DEST) {
      bt.blend = 1;
      bt.fix_alpha = s_blend.fix / 127.f;
    } else if (a == B::SOURCE && b == B::SOURCE && c == B::SOURCE && d == B::SOURCE) {
      bt.blend = 0;
    } else if (a == B::SOURCE && b == B::ZERO_OR_FIXED && c == B::DEST && d == B::DEST) {
      bt.blend = 6;
      bt.color_mult = 0.5f;
    } else {
      bt.blend = 1;
      s_stats.odd_blend++;
    }
  }

  // update_gl_texture
  if (tex_used) {
    bt.tex_id = s_lookup ? s_lookup(s_tex_buf.tbp, s_tex_buf.mt4hh) : 0;
    bt.filt = s_tex_buf.filt;
    bt.clamp_s = s_tex_buf.clamp_s;
    bt.clamp_t = s_tex_buf.clamp_t;
  }
  bt.scissor = s_scissor_enable[s_bucket_idx];
  s_batches.push_back(bt);
  s_stats.batches++;
  s_batch_first = (uint32_t)s_verts.size();
}

// GL get_texture_unit_for_current_reg with TEXTURE_STATE_COUNT = 1.
void claim_texture() {
  if (s_tex_current) {
    return;
  }
  if (s_tex_buf_used) {
    flush();  // flush_from_state_exhaust
  }
  s_tex_buf = s_tex_reg;
  s_tex_buf_used = true;
  s_tex_current = true;
}

void push(const uint8_t rgba[4], const uint32_t vert[4], const float stq[3], bool textured_attrs) {
  Vertex v = {};
  memcpy(v.rgba, rgba, 4);
  v.xyzf[0] = (float)vert[0] / (float)UINT32_MAX;
  v.xyzf[1] = (float)vert[1] / (float)UINT32_MAX;
  v.xyzf[2] = (float)vert[2] / (float)0xffffff;
  v.xyzf[3] = (float)vert[3];
  memcpy(v.stq, stq, 12);
  if (textured_attrs) {
    v.tcc = s_tex_buf.tcc;
    v.decal = s_tex_buf.decal;
    v.fog_enable = s_prim.fge;
    v.use_uv = s_prim.fst;
  }
  v.scissor[0] = s_scissor.x0;
  v.scissor[1] = s_scissor.x1;
  v.scissor[2] = s_scissor.y0;
  v.scissor[3] = s_scissor.y1;
  s_verts.push_back(v);
}

// GL LINE / LINE_STRIP: a quad of width 1 << 19 around the segment (2 triangles, untextured).
void push_line() {
  double p0[3], p1[3];
  for (int i = 0; i < 3; i++) {
    p0[i] = s_pb.b_vert[0][i];
    p1[i] = s_pb.b_vert[1][i];
  }
  double dx = p1[0] - p0[0], dy = p1[1] - p0[1], dz = p1[2] - p0[2];
  const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
  if (len > 0) {
    dx /= len;
    dy /= len;
  }
  // (p1 - p0).normalized() x (0, 0, 1) = (dy, -dx, 0)
  const double nx = dy * (1 << 19), ny = -dx * (1 << 19);
  uint32_t a[4] = {(uint32_t)(p0[0] + nx), (uint32_t)(p0[1] + ny), (uint32_t)p0[2], 0};
  uint32_t b[4] = {(uint32_t)(p1[0] + nx), (uint32_t)(p1[1] + ny), (uint32_t)p1[2], 0};
  uint32_t c[4] = {(uint32_t)(p0[0] - nx), (uint32_t)(p0[1] - ny), (uint32_t)p0[2], 0};
  uint32_t d[4] = {(uint32_t)(p1[0] - nx), (uint32_t)(p1[1] - ny), (uint32_t)p1[2], 0};
  const float z3[3] = {0, 0, 0};
  push(s_pb.b_rgba[0], a, z3, false);
  push(s_pb.b_rgba[0], c, z3, false);
  push(s_pb.b_rgba[1], b, z3, false);
  push(s_pb.b_rgba[1], b, z3, false);
  push(s_pb.b_rgba[0], c, z3, false);
  push(s_pb.b_rgba[1], d, z3, false);
}

// GL handle_xyzf2_common.
void vertex_kick(uint32_t x, uint32_t y, uint32_t z, uint8_t f, bool advance) {
  if (s_verts.size() + 18 > kMaxVerts) {
    s_stats.capped++;
    return;
  }
  if (s_pb.idx > 2) {
    s_pb.idx = 0;
  }
  const int i0 = s_pb.idx;
  s_pb.b_stq[i0][0] = s_pb.st[0];
  s_pb.b_stq[i0][1] = s_pb.st[1];
  s_pb.b_stq[i0][2] = s_pb.Q;
  memcpy(s_pb.b_rgba[i0], s_pb.rgba, 4);
  s_pb.b_vert[i0][0] = x;
  s_pb.b_vert[i0][1] = y;
  s_pb.b_vert[i0][2] = z;
  s_pb.b_vert[i0][3] = f;
  s_pb.idx++;

  claim_texture();

  switch (s_pb.kind) {
    case GsPrim::Kind::SPRITE:
      if (s_pb.idx == 2) {
        const uint32_t* c1 = s_pb.b_vert[0];
        const uint32_t* c2 = s_pb.b_vert[1];
        const float* s1 = s_pb.b_stq[0];
        const float* s2 = s_pb.b_stq[1];
        uint32_t c3[4] = {c1[0], c2[1], c2[2], 0};
        uint32_t c4[4] = {c2[0], c1[1], c2[2], 0};
        float s3[3] = {s1[0], s2[1], s2[2]};
        float s4[3] = {s2[0], s1[1], s2[2]};
        const uint8_t* col = s_pb.b_rgba[1];
        push(s_pb.b_rgba[0], c1, s1, true);
        push(col, c3, s3, true);
        push(col, c2, s2, true);
        push(col, c2, s2, true);
        push(col, c4, s4, true);
        push(s_pb.b_rgba[0], c1, s1, true);
        s_pb.idx = 0;
      }
      break;
    case GsPrim::Kind::TRI_STRIP:
      if (s_pb.idx == 3) {
        s_pb.idx = 0;
      }
      if (s_pb.strip_startup < 3) {
        s_pb.strip_startup++;
      }
      if (s_pb.strip_startup >= 3 && advance) {
        for (int i = 0; i < 3; i++) {
          push(s_pb.b_rgba[i], s_pb.b_vert[i], s_pb.b_stq[i], true);
        }
      }
      break;
    case GsPrim::Kind::TRI:
      if (s_pb.idx == 3) {
        s_pb.idx = 0;
        for (int i = 0; i < 3; i++) {
          push(s_pb.b_rgba[i], s_pb.b_vert[i], s_pb.b_stq[i], true);
        }
      }
      break;
    case GsPrim::Kind::TRI_FAN:
      if (s_pb.strip_startup < 2) {
        s_pb.strip_startup++;
      } else {
        if (s_pb.idx == 3) {
          s_pb.idx = 1;
        }
        for (int i = 0; i < 3; i++) {
          push(s_pb.b_rgba[i], s_pb.b_vert[i], s_pb.b_stq[i], true);
        }
      }
      break;
    case GsPrim::Kind::LINE:
      if (s_pb.idx == 2) {
        push_line();
        s_pb.idx = 0;
      }
      break;
    case GsPrim::Kind::LINE_STRIP:
      if (s_pb.idx == 2) {
        s_pb.idx = 0;
      }
      if (s_pb.strip_startup < 2) {
        s_pb.strip_startup++;
      }
      if (s_pb.strip_startup >= 2 && advance) {
        push_line();
      }
      break;
    default:
      s_stats.bad++;  // GL asserts on points / prim 7
      s_pb.idx = 0;
      break;
  }
}

void handle_prim(uint64_t val) {
  if (s_pb.strip_startup) {
    s_pb.strip_startup = 0;
    s_pb.idx = 0;
  } else if (s_pb.idx > 0) {
    s_stats.bad++;  // GL asserts: half-finished prim
    s_pb.idx = 0;
  }
  GsPrim prim(val);
  if (s_prim.reg != prim || s_blend.abe != prim.abe()) {
    flush();
    s_prim.reg = prim;
    s_prim.tme = prim.tme();
    s_prim.fge = prim.fge();
    s_prim.fst = prim.fst();
    s_blend.abe = prim.abe();
  }
  s_pb.kind = prim.kind();
}

void handle_tex0(uint64_t val) {
  GsTex0 reg(val);
  if (s_tex_reg.reg != reg) {
    s_tex_reg.tbp = reg.tbp0();
    s_tex_reg.mt4hh = reg.psm() == GsTex0::PSM::PSMT4HH;
    s_tex_reg.reg = reg;
    s_tex_reg.tcc = reg.tcc();
    s_tex_reg.decal = reg.tfx() == GsTex0::TextureFunction::DECAL;
    s_tex_current = false;
  }
}

void handle_ad(const uint8_t* data) {
  uint64_t value;
  GsRegisterAddress addr;
  memcpy(&value, data, 8);
  memcpy(&addr, data + 8, sizeof(addr));
  switch (addr) {
    case GsRegisterAddress::ZBUF_1: {
      const bool write = !GsZbuf(value).zmsk();
      if (write != s_test.depth_writes) {
        flush();
        s_test.depth_writes = write;
      }
    } break;
    case GsRegisterAddress::TEST_1: {
      GsTest reg(value);
      if (s_test.reg != reg) {
        flush();
        s_test.reg = reg;
        s_test.ate = reg.alpha_test_enable();
        if (s_test.ate) {
          s_test.atst = reg.alpha_test();
          s_test.aref = reg.aref();
          s_test.afail = reg.afail();
        }
        s_test.ztst = reg.ztest();
      }
    } break;
    case GsRegisterAddress::ALPHA_1: {
      GsAlpha reg(value);
      if (s_blend.reg != reg) {
        flush();
        s_blend.reg = reg;
        s_blend.a = reg.a_mode();
        s_blend.b = reg.b_mode();
        s_blend.c = reg.c_mode();
        s_blend.d = reg.d_mode();
        s_blend.fix = reg.fix();
      }
    } break;
    case GsRegisterAddress::CLAMP_1:
      if (s_tex_reg.clamp_reg != value) {
        s_tex_current = false;
        s_tex_reg.clamp_reg = value;
        s_tex_reg.clamp_s = value & 0b001;
        s_tex_reg.clamp_t = value & 0b100;
      }
      break;
    case GsRegisterAddress::PRIM:
      handle_prim(value);
      break;
    case GsRegisterAddress::TEX1_1: {
      const bool filt = GsTex1(value).mmag();
      if (filt != s_tex_reg.filt) {
        s_tex_reg.filt = filt;
        s_tex_current = false;
      }
    } break;
    case GsRegisterAddress::TEXA: {
      GsTexa reg(value);
      if (s_prim.ta0 != reg.ta0()) {
        flush();
        s_prim.ta0 = reg.ta0();
      }
    } break;
    case GsRegisterAddress::TEX0_1:
      handle_tex0(value);
      break;
    case GsRegisterAddress::RGBAQ:
      memcpy(s_pb.rgba, data, 4);
      memcpy(&s_pb.Q, data + 4, 4);
      break;
    case GsRegisterAddress::SCISSOR_1:
      s_scissor.x0 = (value >> 0) & 0x7ff;
      s_scissor.x1 = (value >> 16) & 0x7ff;
      s_scissor.y0 = (value >> 32) & 0x7ff;
      s_scissor.y1 = (value >> 48) & 0x7ff;
      s_scissor_enable[s_bucket_idx] = true;
      break;
    case GsRegisterAddress::PABE:
    case GsRegisterAddress::TEXCLUT:
    case GsRegisterAddress::FOGCOL:
    case GsRegisterAddress::MIPTBP1_1:
    case GsRegisterAddress::MIPTBP2_1:
    case GsRegisterAddress::TEXFLUSH:
    case GsRegisterAddress::FRAME_1:
    case GsRegisterAddress::COLCLAMP:
      break;
    default:
      s_stats.bad++;  // GL asserts (xyoffset is jak2+, blits are unused)
      break;
  }
}

// GL render_gif. Returns false on data GL would assert on.
bool render_gif(const uint8_t* data, uint32_t size) {
  uint32_t offset = 0;
  bool eop = false;
  while (!eop) {
    if (offset + 16 > size) {
      return false;
    }
    GifTag tag(data + offset);
    offset += 16;
    eop = tag.eop();
    s_stats.packets++;
    GifTag::RegisterDescriptor reg_desc[16];
    const uint32_t nreg = tag.nreg();
    for (uint32_t i = 0; i < nreg; i++) {
      reg_desc[i] = tag.reg(i);
    }
    const auto format = tag.flg();
    if (format == GifTag::Format::PACKED) {
      if (offset + (uint64_t)tag.nloop() * nreg * 16 > size) {
        return false;
      }
      if (tag.pre()) {
        handle_prim(tag.prim());
      }
      for (uint32_t loop = 0; loop < tag.nloop(); loop++) {
        for (uint32_t r = 0; r < nreg; r++) {
          const uint8_t* q = data + offset;
          switch (reg_desc[r]) {
            case GifTag::RegisterDescriptor::AD:
              handle_ad(q);
              break;
            case GifTag::RegisterDescriptor::ST:
              memcpy(&s_pb.st[0], q, 4);
              memcpy(&s_pb.st[1], q + 4, 4);
              memcpy(&s_pb.Q, q + 8, 4);
              break;
            case GifTag::RegisterDescriptor::RGBAQ:
              s_pb.rgba[0] = q[0];
              s_pb.rgba[1] = q[4];
              s_pb.rgba[2] = q[8];
              s_pb.rgba[3] = q[12];
              break;
            case GifTag::RegisterDescriptor::XYZF2: {
              uint32_t x, y;
              uint64_t upper;
              memcpy(&x, q, 4);
              memcpy(&y, q + 4, 4);
              memcpy(&upper, q + 8, 8);
              vertex_kick(x << 16, y << 16, (upper >> 4) & 0xffffff, (uint8_t)(upper >> 36),
                          !(upper & (1ull << 47)));
            } break;
            case GifTag::RegisterDescriptor::XYZ2: {
              uint32_t x, y;
              uint64_t upper;
              memcpy(&x, q, 4);
              memcpy(&y, q + 4, 4);
              memcpy(&upper, q + 8, 8);
              vertex_kick(x << 16, y << 16, (uint32_t)upper, 0, !(upper & (1ull << 47)));
            } break;
            case GifTag::RegisterDescriptor::PRIM: {
              uint64_t v;
              memcpy(&v, q, 8);
              handle_prim(v);
            } break;
            case GifTag::RegisterDescriptor::TEX0_1: {
              uint64_t v;
              memcpy(&v, q, 8);
              handle_tex0(v);
            } break;
            case GifTag::RegisterDescriptor::UV: {
              uint32_t u, v;
              memcpy(&u, q, 4);
              memcpy(&v, q + 4, 4);
              s_pb.st[0] = (float)u;
              s_pb.st[1] = (float)v;
              s_pb.Q = 1;
            } break;
            default:
              s_stats.bad++;
              break;
          }
          offset += 16;
        }
      }
    } else if (format == GifTag::Format::REGLIST) {
      if (offset + (uint64_t)tag.nloop() * nreg * 8 > size) {
        return false;
      }
      for (uint32_t loop = 0; loop < tag.nloop(); loop++) {
        for (uint32_t r = 0; r < nreg; r++) {
          uint64_t v;
          memcpy(&v, data + offset, 8);
          switch (reg_desc[r]) {
            case GifTag::RegisterDescriptor::PRIM:
              handle_prim(v);
              break;
            case GifTag::RegisterDescriptor::RGBAQ:
              memcpy(s_pb.rgba, &v, 4);
              break;
            case GifTag::RegisterDescriptor::XYZF2:
              vertex_kick((uint32_t)(v & 0xffff) << 16, (uint32_t)((v >> 16) & 0xffff) << 16,
                          (v >> 32) & 0xffffff, (uint8_t)(v >> 56), true);
              break;
            default:
              s_stats.bad++;
              break;
          }
          offset += 8;
        }
      }
    } else {
      return false;  // IMAGE (blits) is unused by these buckets
    }
  }
  return true;
}

// VifCode -> DIRECT qwc, 0 for NOP/FLUSHA, -1 for anything else (GL asserts).
int direct_qwc(uint32_t vif) {
  VifCode code(vif);
  switch (code.kind) {
    case VifCode::Kind::NOP:
    case VifCode::Kind::FLUSHA:
      return 0;
    case VifCode::Kind::DIRECT:
      return code.immediate == 0 ? 65536 : code.immediate;
    default:
      return -1;
  }
}

// GL render_vif.
void render_vif(uint32_t vif0, uint32_t vif1, const uint8_t* data, uint32_t size) {
  int qwc = direct_qwc(vif0);
  if (qwc == 0) {
    qwc = direct_qwc(vif1);
  }
  if (qwc < 0) {
    s_stats.bad++;
    return;
  }
  uint32_t off = 0;
  while (off < size) {
    if (qwc) {
      if (off & 0xf) {
        off += 4;  // padding nops
      } else {
        const uint32_t bytes = std::min<uint32_t>((uint32_t)qwc * 16, size - off);
        if (!render_gif(data + off, bytes)) {
          s_stats.bad++;
        }
        off += (uint32_t)qwc * 16;
        qwc = 0;
      }
    } else {
      uint32_t vif;
      memcpy(&vif, data + off, 4);
      off += 4;
      qwc = direct_qwc(vif);
      if (qwc < 0) {
        s_stats.bad++;
        return;
      }
    }
  }
}

// GL DirectRenderer::render for one bucket.
void walk_bucket(const void* data, uint32_t slot, uint32_t next, uint32_t default_regs) {
  reset_state();
  DmaFollower d(data, slot);
  for (int guard = 0; guard < 200000 && !d.ended() && d.current_tag_offset() != next; guard++) {
    auto t = d.read_and_advance();
    if (t.size_bytes) {
      render_vif(t.vif0(), t.vif1(), t.data, t.size_bytes);
    }
    if (d.current_tag_offset() == default_regs) {
      d.read_and_advance();  // cnt
      d.read_and_advance();  // ret
    }
  }
  flush();
}

// ---- per-frame GPU ring, 3 slots like SpriteDk ----
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
    switch_run_logf("[dk] direct ring slot %u: %u KB %s", s_build % kSlots, need >> 10,
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

uint64_t frag_params(Slot* s, const Batch& b, float amin, float amax) {
  uint64_t gpu = 0;
  uint8_t* p = slot_alloc(s, kFragParamBytes, &gpu);
  if (p == nullptr) {
    return 0;
  }
  FragParams f = {};
  f.alpha[0] = amin;
  f.alpha[1] = amax;
  f.alpha[2] = b.greater ? 1.f : 0.f;
  f.alpha[3] = b.ta0;
  f.mult[0] = b.color_mult;
  f.mult[1] = 1.f;
  f.mult[2] = b.textured ? 1.f : 0.f;
  f.mult[3] = b.scissor ? 1.f : 0.f;
  f.fix[0] = b.fix_alpha;
  memcpy(p, &f, sizeof(f));
  return gpu;
}

}  // namespace

void capture_chain(const void* data, uint32_t start_offset, LookupFn lookup) {
  s_verts.clear();
  s_batches.clear();
  s_stats = CaptureStats();
  s_lookup = lookup;
  s_batch_first = 0;
  if (data == nullptr) {
    return;
  }
  DmaFollower d(data, start_offset);
  if (d.current_tag().kind != DmaTag::Kind::CALL) {
    return;  // not a game frame chain
  }
  const uint32_t buckets_base = d.current_tag_offset() + 16;
  // GL OpenGLRenderer::dispatch_buckets: the first CALL's target is the default-regs chain.
  d.read_and_advance();
  const uint32_t default_regs = d.current_tag_offset();
  for (int i = 0; i < 3; i++) {
    s_bucket_idx = i;
    const uint32_t slot = buckets_base + 16 * kJak1Buckets[i];
    walk_bucket(data, slot, slot + 16, default_regs);
  }
}

const CaptureStats& capture_stats() {
  return s_stats;
}
const std::vector<Vertex>& captured_vertices() {
  return s_verts;
}
const std::vector<Batch>& captured_batches() {
  return s_batches;
}
const FrameDraws& frame_draws() {
  return s_frame;
}

void build_frame(const uint8_t fog_rgba[4]) {
  s_frame = FrameDraws();
  memcpy(s_frame.fog_rgba, fog_rgba, 4);
  if (s_batches.empty()) {
    return;
  }
  s_frame.verts = (uint32_t)s_verts.size();
  s_build++;
  const uint32_t vbytes = (uint32_t)(s_verts.size() * sizeof(Vertex));
  const uint32_t need = vbytes + 0x200 + (uint32_t)s_batches.size() * 2 * kFragParamBytes;
  Slot* ring = slot_begin(need);
  uint64_t vgpu = 0, cgpu = 0;
  uint8_t* v = slot_alloc(ring, vbytes, &vgpu);
  uint8_t* c = slot_alloc(ring, sizeof(FrameConsts), &cgpu);
  if (v == nullptr || c == nullptr) {
    s_frame.dropped = (uint32_t)s_batches.size();
    return;
  }
  memcpy(v, s_verts.data(), vbytes);
  s_frame.vert_gpu = vgpu;
  s_frame.vert_bytes = vbytes;
  s_frame.consts_cpu = c;
  s_frame.consts_gpu = cgpu;

  for (const Batch& b : s_batches) {
    if (b.textured && b.tex_id == 0) {
      s_frame.no_tex++;
      continue;
    }
    DrawCmd cmd;
    cmd.first = b.first;
    cmd.count = b.count;
    cmd.tex_id = b.textured ? b.tex_id : 0;
    cmd.depth_test = b.depth_test;
    cmd.depth_op = b.depth_op;
    cmd.depth_write = b.depth_write;
    cmd.blend = b.blend;
    cmd.clamp_s = b.clamp_s;
    cmd.clamp_t = b.clamp_t;
    cmd.filt = b.filt;
    if (b.double_draw) {
      // GL flush_pending double draw: depth write on + [aref, 10], then off + [-10, aref].
      cmd.depth_write = true;
      cmd.frag_gpu = frag_params(ring, b, b.double_aref, 10.f);
      if (cmd.frag_gpu == 0) {
        s_frame.dropped++;
        continue;
      }
      s_frame.draws.push_back(cmd);
      cmd.depth_write = false;
      cmd.frag_gpu = frag_params(ring, b, -10.f, b.double_aref);
    } else {
      cmd.frag_gpu = frag_params(ring, b, b.alpha_min, b.alpha_max);
    }
    if (cmd.frag_gpu == 0) {
      s_frame.dropped++;
      continue;
    }
    s_frame.draws.push_back(cmd);
  }
}

}  // namespace dkdirect
#endif  // __SWITCH__ || DK_HOST_REPLAY
