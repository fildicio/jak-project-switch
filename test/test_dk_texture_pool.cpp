// gtest for dk_texture_pool.h (T19): prove the deko3d texture-store bookkeeping
// (slot ids, level ref-counting, capacity exhaustion, high-water) behaves on the
// Mac before it ever runs on a Switch. (AI-assisted)
//
// dk_texture_pool.h is pure logic and only needs <cstdint>/<unordered_map>/
// <vector>; we deliberately do NOT pull in common/common_types.h (its u128 would
// clash, per the deko3d rules) nor <deko3d.h> (no driver, host-testable).

#include <cstdint>
#include <vector>

#include "game/graphics/deko3d_renderer/dk_texture_pool.h"
#include "gtest/gtest.h"

namespace {

// A tiny texture entry helper so the tests read clearly.
dktex::Entry make_entry() {
  dktex::Entry e;
  e.w = 64;
  e.h = 64;
  e.mips = 1;
  e.fmt = dktex::Fmt::Bc1;
  return e;
}

}  // namespace

// init reserves id 1 (the placeholder): alloc returns ids >= 2; get(kPlaceholderId)
// is nullptr (this skeleton never populates the placeholder slot itself).
TEST(DkTextureStore, InitReservesPlaceholderId) {
  dktex::TextureStore store;
  ASSERT_TRUE(store.init(8));

  // id 0 is never used, id 1 is the reserved placeholder.
  EXPECT_EQ(store.get(0), nullptr);
  EXPECT_EQ(store.get(dktex::TextureStore::kPlaceholderId), nullptr);

  // First real allocation is id 2; the placeholder id (1) is never handed out.
  uint32_t first = store.alloc(make_entry());
  ASSERT_EQ(first, 2u);
  EXPECT_NE(store.get(first), nullptr);
  EXPECT_EQ(store.live_count(), 1u);
}

// A bare release frees a freshly-allocated texture (level_refs was 0 at alloc);
// re-alloc gives a NEW id (ids are never reused) but reuses the freed slot.
TEST(DkTextureStore, AllocFreeRoundTrip) {
  dktex::TextureStore store;
  ASSERT_TRUE(store.init(8));

  uint32_t id = store.alloc(make_entry());
  ASSERT_NE(store.get(id), nullptr);
  EXPECT_EQ(store.live_count(), 1u);

  // A bare release frees it (level_refs 0 -> 0); re-alloc gives a NEW id but
  // reuses the freed slot.
  ASSERT_TRUE(store.release(id));
  EXPECT_EQ(store.get(id), nullptr);
  EXPECT_EQ(store.live_count(), 0u);

  uint32_t again = store.alloc(make_entry());
  ASSERT_NE(store.get(again), nullptr);
  EXPECT_GT(again, id);  // a fresh id -- the old one is dead forever
  EXPECT_EQ(store.live_count(), 1u);
}

// Level ref-counting (the GL "same texture in multiple levels" rule). Add two
// refs, the first release keeps it alive, the second frees it.
TEST(DkTextureStore, LevelRefCountingFreesAtZero) {
  dktex::TextureStore store;
  ASSERT_TRUE(store.init(8));

  uint32_t id = store.alloc(make_entry());
  ASSERT_TRUE(store.add_level_ref(id));
  ASSERT_TRUE(store.add_level_ref(id));
  dktex::Entry* live = store.get(id);
  ASSERT_NE(live, nullptr);
  EXPECT_EQ(live->level_refs, 2u);

  // First release: back to 1, still alive.
  ASSERT_TRUE(store.release(id));
  EXPECT_NE(store.get(id), nullptr);
  EXPECT_EQ(store.get(id)->level_refs, 1u);
  EXPECT_EQ(store.live_count(), 1u);

  // Second release: 0, slot freed.
  ASSERT_TRUE(store.release(id));
  EXPECT_EQ(store.get(id), nullptr);
  EXPECT_EQ(store.live_count(), 0u);
}

// Bad ids (0, an unknown id above the high water, and a double-release of a
// freed slot) all return false and change nothing.
TEST(DkTextureStore, BadIdsReturnFalseAndChangeNothing) {
  dktex::TextureStore store;
  ASSERT_TRUE(store.init(8));

  // id 0 is the invalid sentinel.
  EXPECT_EQ(store.get(0), nullptr);
  EXPECT_FALSE(store.add_level_ref(0));
  EXPECT_FALSE(store.release(0));

  // An id that was never allocated is bad.
  EXPECT_EQ(store.get(999), nullptr);
  EXPECT_FALSE(store.add_level_ref(999));
  EXPECT_FALSE(store.release(999));

  // Allocate one, release it, then double-release and ref the dead id.
  uint32_t id = store.alloc(make_entry());
  ASSERT_TRUE(store.add_level_ref(id));
  ASSERT_TRUE(store.release(id));  // frees (level_refs 1 -> 0)
  EXPECT_EQ(store.get(id), nullptr);

  EXPECT_FALSE(store.release(id));       // already freed: no-op
  EXPECT_FALSE(store.add_level_ref(id));  // dead id: no-op
  EXPECT_EQ(store.live_count(), 0u);  // nothing alive, nothing corrupted
  EXPECT_EQ(store.failed_allocs(), 0);
}

// Capacity exhaustion: once every slot is live, alloc returns 0 and bumps
// failed_allocs (it does NOT evict on its own -- that is the caller's job).
TEST(DkTextureStore, CapacityExhaustionReturnsZero) {
  dktex::TextureStore store;
  // capacity 2 -> only two live slots (ids 2 and 3); the 3rd alloc must fail.
  ASSERT_TRUE(store.init(2));

  ASSERT_NE(store.alloc(make_entry()), 0u);  // id 2
  ASSERT_NE(store.alloc(make_entry()), 0u);  // id 3
  EXPECT_EQ(store.live_count(), 2u);
  EXPECT_EQ(store.failed_allocs(), 0);

  uint32_t full = store.alloc(make_entry());  // 3rd: store is full
  EXPECT_EQ(full, 0u);
  EXPECT_EQ(store.failed_allocs(), 1);
  EXPECT_EQ(store.live_count(), 2u);  // no slot was consumed by the failed alloc

  // One more failed alloc bumps the counter again.
  EXPECT_EQ(store.alloc(make_entry()), 0u);
  EXPECT_EQ(store.failed_allocs(), 2);
}

// A released slot can be handed out again for a new texture (storage reused,
// id is fresh) while ids stay monotonic.
TEST(DkTextureStore, ReusesFreedSlotWithFreshId) {
  dktex::TextureStore store;
  ASSERT_TRUE(store.init(3));

  dktex::Entry e1 = make_entry();
  e1.w = 32;
  dktex::Entry e2 = make_entry();
  e2.w = 128;

  uint32_t a = store.alloc(e1);
  uint32_t b = store.alloc(e2);
  ASSERT_TRUE(store.release(a));
  ASSERT_TRUE(store.release(b));
  EXPECT_EQ(store.live_count(), 0u);

  // Reuse a freed slot for a new id; the entry data is the *new* one.
  dktex::Entry e3 = make_entry();
  e3.w = 256;
  uint32_t c = store.alloc(e3);
  ASSERT_NE(c, 0u);
  EXPECT_GT(c, b);              // fresh id
  EXPECT_EQ(store.get(c)->w, 256u);
  EXPECT_EQ(store.live_count(), 1u);
}

// high_water tracks the maximum simultaneous live count even after the count
// comes back down.
TEST(DkTextureStore, HighWaterTracksPeakLive) {
  dktex::TextureStore store;
  ASSERT_TRUE(store.init(16));

  uint32_t a = store.alloc(make_entry());  // live 1, hw 1
  uint32_t b = store.alloc(make_entry());  // live 2, hw 2
  uint32_t c = store.alloc(make_entry());  // live 3, hw 3
  EXPECT_EQ(store.high_water(), 3u);

  store.release(a);  // live 2, hw stays 3
  store.release(c);  // live 1, hw stays 3
  EXPECT_EQ(store.live_count(), 1u);
  EXPECT_EQ(store.high_water(), 3u);

  uint32_t d = store.alloc(make_entry());  // live 2
  uint32_t e = store.alloc(make_entry());  // live 3, hw stays 3
  uint32_t f = store.alloc(make_entry());  // live 4, hw 4
  ASSERT_NE(f, 0u);
  EXPECT_EQ(store.high_water(), 4u);
  (void)b;
  (void)d;
  (void)e;
}

// init rejects capacities that cannot hold a placeholder + real slots.
TEST(DkTextureStore, InitRejectsTinyCapacity) {
  dktex::TextureStore store;
  EXPECT_FALSE(store.init(0));
  EXPECT_FALSE(store.init(1));
  EXPECT_TRUE(store.init(2));  // minimum valid: 2 slots
  EXPECT_EQ(store.live_count(), 0u);
}
