// gtest for dk_ring.h (T13): prove the per-frame ring allocator works on the Mac
// before it ever runs on a Switch. (AI-assisted)
//
// Note: dk_ring.h is pure logic and only needs <cstdint>; we deliberately do NOT
// include common/common_types.h (its u128 would clash, per the deko3d rules).

#include <cstdint>
#include <vector>

#include "game/graphics/deko3d_renderer/dk_ring.h"
#include "gtest/gtest.h"

namespace {

// Fake GPU base address (well inside a plausible GPU address range).
constexpr uint64_t kGpuBase = 0x100000000ull;

// A 1024-byte buffer split into 2 in-flight frame segments of 512 bytes each.
// 256 * 2 frames = 512 <= 1024, so init() succeeds and each segment is 512 bytes.
struct Mem {
  // 1024-byte buffer; 2 segments of 512 bytes each. Sized in the constructor so this
  // compiles under both C++17 and C++20 (in-class copy-init with parens is C++20 only).
  Mem() : data(1024u) {}
  std::vector<uint8_t> data;
  uint8_t* base() { return data.data(); }
};

}  // namespace

TEST(DkRing, AlignsOffsets) {
  dkring::FrameRing ring;
  Mem mem;
  ring.init(mem.base(), kGpuBase, static_cast<uint32_t>(mem.data.size()), 2);
  ASSERT_EQ(ring.segment_size(), 512u);

  ring.begin_frame(0);
  dkring::Alloc a0 = ring.alloc(10, 256);
  ASSERT_TRUE(a0.ok());
  EXPECT_EQ(a0.offset, 0u);
  EXPECT_EQ(a0.gpu, kGpuBase + 0u);
  EXPECT_EQ(a0.cpu, mem.base() + 0u);

  dkring::Alloc a1 = ring.alloc(10, 256);
  ASSERT_TRUE(a1.ok());
  EXPECT_EQ(a1.offset, 256u);
  EXPECT_EQ(a1.gpu, kGpuBase + 256u);
  EXPECT_EQ(a1.cpu, mem.base() + 256u);
}

TEST(DkRing, SegmentsPerFrame) {
  dkring::FrameRing ring;
  Mem mem;
  ring.init(mem.base(), kGpuBase, static_cast<uint32_t>(mem.data.size()), 2);
  const uint32_t half = static_cast<uint32_t>(mem.data.size()) / 2u;  // 512

  ring.begin_frame(0);
  dkring::Alloc f0 = ring.alloc(10, 256);
  ASSERT_TRUE(f0.ok());

  ring.begin_frame(1);
  dkring::Alloc f1 = ring.alloc(10, 256);
  ASSERT_TRUE(f1.ok());

  // Frame 0 lands in the first half, frame 1 in the second half.
  EXPECT_LT(f0.offset, half);
  EXPECT_GE(f1.offset, half);
  EXPECT_NE(f0.offset, f1.offset);
}

TEST(DkRing, BeginFrameResets) {
  dkring::FrameRing ring;
  Mem mem;
  ring.init(mem.base(), kGpuBase, static_cast<uint32_t>(mem.data.size()), 2);

  ring.begin_frame(0);
  EXPECT_TRUE(ring.alloc(20, 256).ok());  // advance the cursor within segment 0

  // begin_frame(2) is the same segment as frame 0 (2 % 2 == 0) and must reset the
  // cursor to that segment's start.
  ring.begin_frame(2);
  dkring::Alloc a = ring.alloc(10, 256);
  ASSERT_TRUE(a.ok());
  EXPECT_EQ(a.offset, 0u);  // == segment 0 start
}

TEST(DkRing, FullSegmentFails) {
  dkring::FrameRing ring;
  Mem mem;
  ring.init(mem.base(), kGpuBase, static_cast<uint32_t>(mem.data.size()), 2);
  EXPECT_EQ(ring.segment_size(), 512u);

  ring.begin_frame(0);
  ASSERT_TRUE(ring.alloc(400, 256).ok());  // 256-aligned: 0..400 fills most of the seg
  dkring::Alloc over = ring.alloc(200, 256);  // would run past the 512 boundary
  EXPECT_FALSE(over.ok());
  EXPECT_EQ(ring.failures(), 1u);

  // The next frame's segment (segment 1) must be untouched: its first alloc still
  // starts exactly at the segment start (offset 512).
  ring.begin_frame(1);
  dkring::Alloc seg1 = ring.alloc(10, 256);
  ASSERT_TRUE(seg1.ok());
  EXPECT_EQ(seg1.offset, 512u);
  EXPECT_EQ(ring.failures(), 1u);  // the good alloc on frame 1 adds no failure
}

TEST(DkRing, HighWaterTracks) {
  dkring::FrameRing ring;
  Mem mem;
  ring.init(mem.base(), kGpuBase, static_cast<uint32_t>(mem.data.size()), 2);

  ring.begin_frame(0);
  ASSERT_TRUE(ring.alloc(300, 256).ok());
  EXPECT_EQ(ring.high_water(), 300u);

  // A smaller frame must not reduce the high water mark.
  ring.begin_frame(1);
  ASSERT_TRUE(ring.alloc(100, 256).ok());
  EXPECT_EQ(ring.used(), 100u);
  EXPECT_EQ(ring.high_water(), 300u);

  // A later, larger allocation pushes the high water mark up.
  ring.begin_frame(0);
  ASSERT_TRUE(ring.alloc(450, 256).ok());
  EXPECT_EQ(ring.high_water(), 450u);
}

TEST(DkRing, BadInitUnusable) {
  dkring::FrameRing ring;
  Mem mem;
  // frames == 0 -> the ring is unusable; alloc must fail without touching memory.
  ring.init(mem.base(), kGpuBase, static_cast<uint32_t>(mem.data.size()), 0);
  EXPECT_EQ(ring.segment_size(), 0u);
  dkring::Alloc a = ring.alloc(10, 256);
  EXPECT_FALSE(a.ok());
  EXPECT_EQ(a.cpu, nullptr);
  EXPECT_EQ(ring.used(), 0u);
}
