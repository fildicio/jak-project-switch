// test_dk_ocean_common.cpp - host tests for the dk ocean common recorder (T93,
// AI-assisted). The recorder is plain CPU decode code, so it runs on the host
// build directly (no deko3d device involved).

#include "game/graphics/deko3d_renderer/dk_ocean_common.h"

#include <gtest/gtest.h>

#include <vector>

namespace dkocean {
// Friend of CommonRecorder (declared in dk_ocean_common.h): exposes private
// decode state to these tests only. No runtime effect on the game build.
struct OceanRecorderTestPeer {
  static size_t vertex_size() { return sizeof(CommonRecorder::Vertex); }
  static u32 next_free_vertex(const CommonRecorder& r) { return r.m_next_free_vertex; }
  static u32 current_bucket(const CommonRecorder& r) { return r.m_current_bucket; }
  static size_t vertices_size(const CommonRecorder& r) { return r.m_vertices.size(); }
  static size_t indices_size(const CommonRecorder& r, int bucket) {
    return r.m_indices[bucket].size();
  }
  // T96 (AI-assisted): used by the kick regression tests below.
  static u32 next_free_index(const CommonRecorder& r, int bucket) {
    return r.m_next_free_index[bucket];
  }
  static u32 index_at(const CommonRecorder& r, int bucket, u32 i) {
    return r.m_indices[bucket][i];
  }
  static const CommonRecorder::Vertex* vertices(const CommonRecorder& r) {
    return r.m_vertices.data();
  }
};
}  // namespace dkocean

using dkocean::CommonRecorder;
using dkocean::OceanRecorderTestPeer;

TEST(OceanDkCommon, VertexLayout) {
  // 32 bytes, same as the GL renderer's upload layout (static_assert in the
  // header enforces it at compile time; this documents it at test time).
  EXPECT_EQ(OceanRecorderTestPeer::vertex_size(), 32u);
}

TEST(OceanDkCommon, InitResets) {
  CommonRecorder r;
  r.init_for_near();
  EXPECT_EQ(OceanRecorderTestPeer::next_free_vertex(r), 0u);
  EXPECT_EQ(OceanRecorderTestPeer::current_bucket(r), 0u);  // RGB_TEXTURE
  // T96 (AI-assisted): the decode buffers must be allocated here. The T93
  // build shipped an empty constructor AND pinned it with EXPECT_EQ(..., 0u):
  // on console the first ocean kick wrote through the null data pointers
  // (gk_fatal 2026-10-23, data abort far=0x0 in handle_near_vertex_gif_data_strip).
  EXPECT_EQ(OceanRecorderTestPeer::vertices_size(r), (size_t)CommonRecorder::kInitialCapacity);
  for (int b = 0; b < 3; b++) {
    EXPECT_EQ(OceanRecorderTestPeer::indices_size(r, b), (size_t)CommonRecorder::kInitialCapacity);
  }
  r.init_for_mid();
  EXPECT_EQ(OceanRecorderTestPeer::next_free_vertex(r), 0u);
}

TEST(OceanDkCommon, ReverseIndices) {
  // odd count
  std::vector<u32> odd = {1, 2, 3, 4, 5};
  dkocean::reverse_indices(odd.data(), odd.size());
  EXPECT_EQ(odd, (std::vector<u32>{5, 4, 3, 2, 1}));
  // even count
  std::vector<u32> even = {1, 2, 3, 4};
  dkocean::reverse_indices(even.data(), even.size());
  EXPECT_EQ(even, (std::vector<u32>{4, 3, 2, 1}));
  // single element
  std::vector<u32> one = {9};
  dkocean::reverse_indices(one.data(), one.size());
  EXPECT_EQ(one, (std::vector<u32>{9}));
  // zero count must not crash (the `if (count)` guard)
  dkocean::reverse_indices(nullptr, 0);
  SUCCEED();
}

// ---------------------------------------------------------------------------
// T96 (AI-assisted): kick regression tests. The T93 build never executed a
// kick on host or console before shipping, so the empty-constructor null
// writes were only caught by a console crash. These craft minimal GIF kicks
// in the project's own GifTag layout (common/dma/gs.h: nloop 0:14, eop 15,
// pre 46, prim 47..56, flg 58:59, nreg 60:63, regs packed in data[1]) and
// decode them end to end.
// ---------------------------------------------------------------------------
namespace {

// GsPrim::Kind values (common/dma/gs.h).
constexpr u32 kPrimTriStrip = 4;
constexpr u32 kPrimTriFan = 5;
constexpr u32 kAbe = 1u << 6;  // GsPrim::abe(), alpha-blend enable bit

// ST=2, RGBAQ=1, XYZF2=4 (GifTag::RegisterDescriptor), packed 4 bits each.
constexpr u64 kRegsStRgbaqXyzf = 2 | (1ull << 4) | (4ull << 8);

void append_qw(std::vector<u8>& b, u64 lo, u64 hi = 0) {
  b.insert(b.end(), (const u8*)&lo, (const u8*)&lo + 8);
  b.insert(b.end(), (const u8*)&hi, (const u8*)&hi + 8);
}

void append_tag(std::vector<u8>& b, u32 nloop, u32 prim, bool pre, bool eop, u64 regs) {
  u64 lo = (u64)nloop | (u64(eop) << 15) | (u64(pre) << 46) | (u64(prim) << 47) |
           (u64(0) << 58) /* PACKED */ | (u64(3) << 60);
  append_qw(b, lo, regs);
}

// One packed vertex in ST / RGBAQ / XYZF order. adc bit 47 controls the
// auto-advance cut in the strip handler. Bytes land little-endian: the
// handlers read st floats from bytes 0/4/8, rgba bytes at 0/4/8/12 of the
// rgbaq qword, and x/y u32 + upper u64 from the xyzf qword.
void append_vert(std::vector<u8>& b, u32 adc) {
  append_qw(b, 0x400000003f800000ull, 0x0000000040400000ull);  // st: 1.0f 2.0f 3.0f
  append_qw(b, 0x8877665544332211ull, 0x0807060504030201ull);  // rgbaq bytes 11..88
  u64 upper = (u64(0x100) << 4)         // z
              | (u64(0x20) << 36)       // fog
              | (u64(adc) << 47);       // adc
  append_qw(b, 0x4000000040000000ull /* x, y */, upper);
}

}  // namespace

// The exact console crash path: kick_from_mid -> nreg 3 strip
// (handle_near_vertex_gif_data_strip). With the T93 empty constructor this
// wrote through null pointers; it must decode cleanly now.
TEST(OceanDkCommon, KickFromMidDecodesStrip) {
  CommonRecorder r;
  r.init_for_mid();
  std::vector<u8> buf;
  // abe set -> kick_from_mid routes the strip to the ALPHA bucket (1).
  append_tag(buf, 4 /*nloop*/, kPrimTriStrip | kAbe, /*pre=*/true, /*eop=*/true,
             kRegsStRgbaqXyzf);
  for (int i = 0; i < 4; i++) {
    append_vert(buf, /*adc=*/0);
  }
  r.kick_from_mid(buf.data());

  EXPECT_EQ(OceanRecorderTestPeer::next_free_vertex(r), 4u);
  EXPECT_EQ(OceanRecorderTestPeer::current_bucket(r), 1u);  // abe=1 -> ALPHA
  // UINT32_MAX restart + one index per vertex (no adc).
  EXPECT_EQ(OceanRecorderTestPeer::next_free_index(r, 1), 5u);
  EXPECT_EQ(OceanRecorderTestPeer::index_at(r, 1, 0), UINT32_MAX);
  EXPECT_EQ(OceanRecorderTestPeer::index_at(r, 1, 1), 0u);
  EXPECT_EQ(OceanRecorderTestPeer::index_at(r, 1, 4), 3u);
  // Decoded vertex 0 (stq, rgba, fog).
  const auto* v = OceanRecorderTestPeer::vertices(r);
  EXPECT_EQ(v[0].stq[0], 1.0f);
  EXPECT_EQ(v[0].stq[1], 2.0f);
  EXPECT_EQ(v[0].stq[2], 3.0f);
  EXPECT_EQ(v[0].rgba[0], 0x11);
  EXPECT_EQ(v[0].rgba[3], 0x05);
  EXPECT_EQ(v[0].fog, 0x20);
}

// kick_from_near -> nreg 3 fan (handle_near_vertex_gif_data_fan): the
// draw-fans-with-strips emulation writes 4 indices for the 3rd+ vertices.
TEST(OceanDkCommon, KickFromNearDecodesFan) {
  CommonRecorder r;
  r.init_for_near();
  std::vector<u8> buf;
  append_tag(buf, 3 /*nloop*/, kPrimTriFan, /*pre=*/true, /*eop=*/true, kRegsStRgbaqXyzf);
  for (int i = 0; i < 3; i++) {
    append_vert(buf, /*adc=*/0);
  }
  r.kick_from_near(buf.data());

  EXPECT_EQ(OceanRecorderTestPeer::next_free_vertex(r), 3u);
  EXPECT_EQ(OceanRecorderTestPeer::next_free_index(r, 0), 4u);
  EXPECT_EQ(OceanRecorderTestPeer::index_at(r, 0, 0), UINT32_MAX);
  EXPECT_EQ(OceanRecorderTestPeer::index_at(r, 0, 3), 0u);  // ind_of_fan_start
}

// Three strips whose accumulated demand exceeds kInitialCapacity: the T96
// ensure_capacity guard must grow the buffers (double) instead of writing out
// of bounds. Each strip reserves worst-case 3*loop+1 indices, so the first
// strip already needs 9001 > 8192 and doubles the buffers once to 16384;
// the remaining strips must fit without another growth.
TEST(OceanDkCommon, GrowsBeyondInitialCapacity) {
  CommonRecorder r;
  r.init_for_mid();
  constexpr u32 kPerStrip = 3000;
  std::vector<u8> buf;
  for (int s = 0; s < 3; s++) {
    append_tag(buf, kPerStrip, kPrimTriStrip, /*pre=*/true, /*eop=*/s == 2, kRegsStRgbaqXyzf);
    for (u32 i = 0; i < kPerStrip; i++) {
      append_vert(buf, /*adc=*/0);
    }
  }
  r.kick_from_mid(buf.data());

  EXPECT_EQ(OceanRecorderTestPeer::next_free_vertex(r), 3 * kPerStrip);
  EXPECT_EQ(OceanRecorderTestPeer::vertices_size(r), (size_t)CommonRecorder::kInitialCapacity * 2);
  // 3 strips x (UINT32_MAX restart + one index per vertex).
  EXPECT_EQ(OceanRecorderTestPeer::next_free_index(r, 0), 3 * (kPerStrip + 1));
  EXPECT_EQ(OceanRecorderTestPeer::index_at(r, 0, 0), UINT32_MAX);
  EXPECT_EQ(OceanRecorderTestPeer::index_at(r, 0, OceanRecorderTestPeer::next_free_index(r, 0) - 1),
            3 * kPerStrip - 1);
}
