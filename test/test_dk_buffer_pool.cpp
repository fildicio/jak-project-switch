// gtest for dk_buffer_pool.h (T17): prove the deko3d FIX 33 port pools, reuses
// and reclaims correctly on the Mac before it ever runs on a Switch. (AI-assisted)
//
// dk_buffer_pool.h is pure logic and only needs <cstdint>/<functional>/
// <unordered_map>/<vector>; we deliberately do NOT include common/common_types.h
// (its u128 would clash, per the deko3d rules).

#include <cstdint>
#include <vector>

#include "game/graphics/deko3d_renderer/dk_buffer_pool.h"
#include "gtest/gtest.h"

namespace {

constexpr uint32_t kSizeClass = 256u * 1024;  // 256 KB, same as GpuBufferPool
constexpr uint64_t kGpuBase = 0x200000000ull;  // fake GPU base

}  // namespace

// First acquire on a fresh pool carves a new chunk from the bump at offset 0 and
// rounds the request up to a whole size class.
TEST(DkBufferPool, CarvesOnFirstAcquire) {
  const uint32_t region = 4u * 1024u * 1024u;  // 4 MB
  std::vector<uint8_t> mem(region);
  dkpool::BufferPool pool;
  pool.init(mem.data(), kGpuBase, region);

  EXPECT_EQ(pool.outstanding(), 0u);
  EXPECT_EQ(pool.pooled_chunks(), 0u);
  EXPECT_EQ(pool.region_unused_bytes(), region);

  // 200 KB request rounds up to one 256 KB size class.
  dkpool::PoolBuffer b = pool.acquire(200u * 1024u);
  ASSERT_TRUE(b.ok());
  EXPECT_EQ(b.id, 1u);                      // ids start at 1 and are monotonic
  EXPECT_EQ(b.cpu, mem.data() + 0u);        // carved at offset 0
  EXPECT_EQ(b.gpu, kGpuBase + 0u);
  EXPECT_EQ(b.capacity, kSizeClass);         // 256 KB size class
  EXPECT_EQ(pool.outstanding(), 1u);
  EXPECT_EQ(pool.pooled_chunks(), 0u);
  EXPECT_EQ(pool.region_unused_bytes(), region - kSizeClass);
}

// A released chunk is reused by the next acquire of a close-enough size and,
// crucially, hands back the SAME cpu address (the whole point of FIX 33: no
// re-allocation of the underlying storage).
TEST(DkBufferPool, ReleaseReuseSameAddress) {
  const uint32_t region = 2u * 1024u * 1024u;  // 2 MB (room to also carve a 2nd class)
  std::vector<uint8_t> mem(region);
  dkpool::BufferPool pool;
  pool.init(mem.data(), kGpuBase, region);

  dkpool::PoolBuffer a = pool.acquire(200u * 1024u);  // 256 KB class at offset 0
  ASSERT_TRUE(a.ok());
  const uint8_t* first_cpu = a.cpu;
  pool.release(a.id);

  EXPECT_EQ(pool.pooled_chunks(), 1u);
  EXPECT_EQ(pool.pooled_bytes(), kSizeClass);
  EXPECT_EQ(pool.outstanding(), 0u);

  // 100 KB also rounds to the 256 KB class, so the same chunk is reused.
  dkpool::PoolBuffer b = pool.acquire(100u * 1024u);
  ASSERT_TRUE(b.ok());
  EXPECT_EQ(b.id, 2u);            // a different id, same storage
  EXPECT_EQ(b.cpu, first_cpu);    // the reused chunk keeps the original address
  EXPECT_EQ(b.capacity, kSizeClass);
  EXPECT_EQ(pool.pooled_chunks(), 0u);
  EXPECT_EQ(pool.outstanding(), 1u);
}

// The <=2x rule: a 1 MB-class chunk must NOT be reused to satisfy a 100 KB
// request (1 MB > 2 * 256 KB), so a brand-new chunk is carved instead and the
// 1 MB chunk stays pooled.
TEST(DkBufferPool, OversizedReuseRefused) {
  const uint32_t region = 4u * 1024u * 1024u;  // room for the 1 MB + a carved 256 KB
  std::vector<uint8_t> mem(region);
  dkpool::BufferPool pool;
  pool.init(mem.data(), kGpuBase, region);

  // 800 KB rounds up to a 1 MB size class; release it so it is a 1 MB free chunk.
  dkpool::PoolBuffer big = pool.acquire(800u * 1024u);
  ASSERT_TRUE(big.ok());
  EXPECT_EQ(big.capacity, 1024u * 1024u);  // 1 MB
  pool.release(big.id);
  ASSERT_EQ(pool.pooled_chunks(), 1u);

  // 100 KB wants a 256 KB class; the only free chunk is 1 MB, which is > 2x the
  // want, so it must be refused and a fresh 256 KB chunk carved instead.
  dkpool::PoolBuffer small = pool.acquire(100u * 1024u);
  ASSERT_TRUE(small.ok());
  EXPECT_EQ(small.capacity, kSizeClass);   // carved 256 KB, not the reused 1 MB
  EXPECT_EQ(small.id, 2u);                 // a new id, not the released one
  // `big` was the FIRST carve (offset 0); `small` carves at the current bump
  // (1 MB), since bump_ never moves down after a release.
  EXPECT_EQ(small.cpu, mem.data() + 1024u * 1024u);
  EXPECT_EQ(big.cpu, mem.data() + 0u);
  // The 1 MB chunk is still pooled (untouched).
  EXPECT_EQ(pool.pooled_chunks(), 1u);
}

// Id 0, unknown ids and a double release are all ignored; the live set and the
// pooled set are unchanged (defensive against the old hfrag double-delete class).
TEST(DkBufferPool, BadReleasesIgnored) {
  const uint32_t region = 1u * 1024u * 1024u;  // 1 MB
  std::vector<uint8_t> mem(region);
  dkpool::BufferPool pool;
  pool.init(mem.data(), kGpuBase, region);

  dkpool::PoolBuffer a = pool.acquire(200u * 1024u);  // id 1, 256 KB class
  ASSERT_TRUE(a.ok());

  pool.release(0);        // id 0 is the invalid sentinel: ignored
  pool.release(9999);     // unknown id: ignored
  EXPECT_EQ(pool.outstanding(), 1u);
  EXPECT_EQ(pool.pooled_chunks(), 0u);

  pool.release(a.id);     // first (legitimate) release of id 1
  EXPECT_EQ(pool.outstanding(), 0u);
  EXPECT_EQ(pool.pooled_chunks(), 1u);

  pool.release(a.id);     // double release: id 1 is no longer live, ignored
  EXPECT_EQ(pool.outstanding(), 0u);
  EXPECT_EQ(pool.pooled_chunks(), 1u);     // still just the one free chunk
  EXPECT_EQ(pool.pooled_bytes(), kSizeClass);
}

// When the region is exhausted, acquire() runs the reclaim callback; a callback
// that frees a live chunk returns true and the retry reuses the freed chunk.
TEST(DkBufferPool, ReclaimFreesAndRetries) {
  const uint32_t region = 1u * 1024u * 1024u;  // 1 MB
  std::vector<uint8_t> mem(region);
  dkpool::BufferPool pool;
  pool.init(mem.data(), kGpuBase, region);

  // Fill the 1 MB region with four 256 KB chunks (4 * 256 KB = 1 MB exactly).
  dkpool::PoolBuffer a = pool.acquire(200u * 1024u);  // id 1, offset 0
  dkpool::PoolBuffer b = pool.acquire(200u * 1024u);  // id 2, offset 256 KB
  dkpool::PoolBuffer c = pool.acquire(200u * 1024u);  // id 3, offset 512 KB
  dkpool::PoolBuffer d = pool.acquire(200u * 1024u);  // id 4, offset 768 KB
  ASSERT_TRUE(a.ok());
  ASSERT_TRUE(b.ok());
  ASSERT_TRUE(c.ok());
  ASSERT_TRUE(d.ok());
  EXPECT_EQ(pool.region_unused_bytes(), 0u);           // region is full
  EXPECT_EQ(pool.failed_allocations(), 0);

  // Next acquire: best_fit finds nothing free and the region is full, so the
  // reclaim callback must free a live chunk. It releases `a` (id 1) and returns
  // true, then the retry reuses `a`'s freed 256 KB chunk.
  bool once = false;
  pool.set_reclaim_callback([&pool, &a, &once]() {
    if (once) {
      return false;
    }
    pool.release(a.id);  // free the 256 KB chunk `a` occupied
    once = true;
    return true;
  });

  dkpool::PoolBuffer e = pool.acquire(200u * 1024u);
  ASSERT_TRUE(e.ok());
  EXPECT_TRUE(once);
  EXPECT_EQ(e.id, 5u);                 // a new id (ids are never reused)
  EXPECT_EQ(e.cpu, a.cpu);             // reuses `a`'s freed chunk address
  EXPECT_EQ(pool.failed_allocations(), 0);
  EXPECT_EQ(pool.pooled_chunks(), 0u);  // the freed chunk was reclaimed, not pooled
}

// When the reclaim callback can free nothing (returns false), acquire gives up
// and counts the failure exactly once per exhausted attempt.
TEST(DkBufferPool, ReclaimFalseCountsFailure) {
  const uint32_t region = 1u * 1024u * 1024u;  // 1 MB
  std::vector<uint8_t> mem(region);
  dkpool::BufferPool pool;
  pool.init(mem.data(), kGpuBase, region);

  // Fill the 1 MB region with four 256 KB chunks so no further carve is possible.
  for (int i = 0; i < 4; ++i) {
    dkpool::PoolBuffer b = pool.acquire(200u * 1024u);
    ASSERT_TRUE(b.ok()) << "carve " << i;
  }
  EXPECT_EQ(pool.region_unused_bytes(), 0u);

  // Reclaim always fails: the next acquire must fail and count one failure.
  pool.set_reclaim_callback([]() { return false; });
  dkpool::PoolBuffer f = pool.acquire(200u * 1024u);
  EXPECT_FALSE(f.ok());
  EXPECT_EQ(f.id, 0u);
  EXPECT_EQ(f.cpu, nullptr);
  EXPECT_EQ(pool.failed_allocations(), 1);

  // A second exhausted acquire counts a second failure (the region is still full).
  dkpool::PoolBuffer g = pool.acquire(200u * 1024u);
  EXPECT_FALSE(g.ok());
  EXPECT_EQ(pool.failed_allocations(), 2);
}

// Telemetry after a release/acquire cycle is internally consistent.
TEST(DkBufferPool, TelemetrySane) {
  const uint32_t region = 2u * 1024u * 1024u;  // 2 MB
  std::vector<uint8_t> mem(region);
  dkpool::BufferPool pool;
  pool.init(mem.data(), kGpuBase, region);

  // 300 KB rounds up to a 512 KB class (2 * 256 KB); 100 KB to a 256 KB class.
  dkpool::PoolBuffer a = pool.acquire(300u * 1024u);
  dkpool::PoolBuffer b = pool.acquire(100u * 1024u);
  ASSERT_TRUE(a.ok());
  ASSERT_TRUE(b.ok());
  EXPECT_EQ(a.capacity, 2u * kSizeClass);  // 512 KB
  EXPECT_EQ(b.capacity, kSizeClass);       // 256 KB
  EXPECT_EQ(pool.outstanding(), 2u);
  EXPECT_EQ(pool.pooled_chunks(), 0u);
  EXPECT_EQ(pool.region_unused_bytes(), region - 3u * kSizeClass);  // 2 MB - 768 KB = 448 KB

  pool.release(a.id);
  EXPECT_EQ(pool.pooled_chunks(), 1u);
  EXPECT_EQ(pool.pooled_bytes(), 2u * kSizeClass);  // the 512 KB chunk is pooled
  EXPECT_EQ(pool.outstanding(), 1u);
  EXPECT_EQ(pool.failed_allocations(), 0);
}
