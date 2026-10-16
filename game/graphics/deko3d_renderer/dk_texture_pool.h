#pragma once
// deko3d texture store (AI-assisted, T19). The bookkeeping half of the planned
// TexturePoolDeko3D (P2.2): a fixed-capacity store of GPU texture slots with
// per-texture level ref-counts. Pure logic, host-testable like dk_buffer_pool.h
// and dk_ring.h -- NO <deko3d.h> include, NO __SWITCH__ guard.
//
// Mirrors the game-facing semantics documented in game/graphics/texture/TexturePool.h
// (the "three layers" of the PC port) and, crucially, this challenge: "The same
// texture may appear in multiple levels, both of which can be loaded at the same
// time." When level B loads a texture that level A already holds, it does NOT
// re-upload it -- it adds a level ref (add_level_ref). When a level unloads it
// takes a ref back (release); only the level that drives the count to 0 frees the
// slot. That is the GPU-side of FIX 33's "dedup by identity, don't re-churn on
// area transitions" (same lesson as dk_buffer_pool.h).
//
// Two independent axes:
//   - id      a monotonic, never-reused handle. 0 is invalid / the failed-acquire
//             sentinel; 1 (kPlaceholderId) is reserved like the GL placeholder
//             texture (a 16x16 checkerboard standing in for not-yet-loaded
//             textures) and is never handed out by alloc -- alloc starts at 2.
//   - slot    a fixed-capacity storage slot. Releasing a texture frees its slot
//             (so the next alloc reuses the storage) but never its id, so a freed
//             id can never alias a still-live texture (the old hfrag double-delete
//             / dangling-pointer bug class).
//
// alloc does NOT evict on its own. When the store is full it returns 0 and bumps
// failed_allocs; the loader's reclaim callback owns eviction and retries (the same
// contract as GpuBufferPool / dk_buffer_pool.h).
//
// BCn textures (v44 fr3) will later upload DIRECT from the card bytes with zero
// CPU decode, so there is no conversion/format hook here -- Fmt is only recorded,
// to be consumed by the later GPU task that creates the DkImage and descriptor
// handles.
//
// Not thread-safe: render thread only (every loader GPU stage runs there).
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace dktex {

// GPU format a texture will be created as, decided per texture later.
enum class Fmt : uint8_t { Rgba8, Bc1, Bc2, Bc3, Bc7 };

// One texture's bookkeeping.
struct Entry {
  uint16_t w = 0, h = 0;
  uint8_t mips = 1;
  Fmt fmt = Fmt::Rgba8;
  uint32_t level_refs = 0;  // same texture loaded by N levels; freed at 0 (FIX 33)
  bool in_use = false;
};

class TextureStore {
 public:
  static constexpr uint32_t kPlaceholderId = 1;  // id 1 reserved, like the GL placeholder

  // Size the store to `capacity` slots. False on capacity < 2 (we need at least
  // one real slot beyond the reserved placeholder/invalid ids) or OOM. id 0 is
  // never used; id 1 stays reserved.
  bool init(uint32_t capacity);

  // Hand out the next id (>= 2) for a new texture, copying e into a slot.
  // Returns 0 when the store is full (no slot free); on 0, no id/slot is
  // consumed and failed_allocs++. alloc does NOT evict -- the caller's reclaim
  // callback frees something and retries (FIX 33, same as GpuBufferPool).
  uint32_t alloc(const Entry& e);

  // Add a level ref: a second level loaded the same texture without re-uploading.
  // False on a bad / unknown / already-freed id.
  bool add_level_ref(uint32_t id);

  // Drop one level ref. When the count hits 0 the slot is freed and the id is
  // dead forever (never reused, so it can't alias anything still live). False on
  // a bad / unknown / already-freed id (so a double-release is a no-op, never a
  // second free / an underflow).
  bool release(uint32_t id);

  // Live-texture lookup. nullptr for id 0, for the reserved placeholder id 1 (never
  // populated by this skeleton), for any unknown id, and for any already-freed id.
  Entry* get(uint32_t id);

  // Telemetry.
  uint32_t live_count() const;    // textures live right now
  uint32_t high_water() const;    // max simultaneous live_count ever reached
  int failed_allocs() const;      // alloc() calls that returned 0 (store was full)

 private:
  uint32_t m_capacity = 0;
  std::vector<Entry> m_entries;                          // slots, indexed 0..capacity-1
  std::vector<uint32_t> m_free;                          // free slot indices (a LIFO stack)
  std::unordered_map<uint32_t, uint32_t> m_id_to_slot;  // live id -> slot index
  uint32_t m_next_id = 0;    // next id to hand out (starts at 2)
  uint32_t m_live = 0;       // live entry count
  uint32_t m_high_water = 0;  // peak live count
  int m_failed = 0;           // count of full (failed) allocs
};

// A fresh store has no slots; init() sizes the backing storage exactly once (so
// &entries[slot] pointers stay stable for the life of the store -- we never resize
// m_entries after init).
inline bool TextureStore::init(uint32_t capacity) {
  if (capacity < 2) {
    return false;  // need at least one real slot beyond the reserved placeholder/0 ids
  }
  m_capacity = capacity;
  m_entries.assign(capacity, Entry{});
  m_free.clear();
  m_free.reserve(capacity);
  // Every slot starts free. Slot index and id are unrelated, so slot 0 is a
  // perfectly normal live slot once it is handed out.
  for (uint32_t i = 0; i < capacity; ++i) {
    m_free.push_back(i);
  }
  m_id_to_slot.clear();
  m_next_id = kPlaceholderId + 1;  // 2 -- id 1 (the placeholder) stays reserved
  m_live = 0;
  m_high_water = 0;
  m_failed = 0;
  return true;
}

// Carve the next id for a new texture. No eviction: when the live count has
// reached capacity there is no free slot, so we fail and let the caller's
// reclaim callback free something and retry.
inline uint32_t TextureStore::alloc(const Entry& e) {
  if (m_live >= m_capacity) {
    m_failed++;  // full -- caller must evict (its reclaim callback) and retry
    return 0;
  }
  const uint32_t slot = m_free.back();
  m_free.pop_back();
  const uint32_t id = m_next_id++;
  m_entries[slot] = e;
  m_entries[slot].in_use = true;
  m_id_to_slot[id] = slot;
  if (++m_live > m_high_water) {
    m_high_water = m_live;
  }
  return id;
}

inline bool TextureStore::add_level_ref(uint32_t id) {
  auto it = m_id_to_slot.find(id);
  if (it == m_id_to_slot.end()) {
    return false;  // bad / unknown / already-freed id
  }
  m_entries[it->second].level_refs++;
  return true;
}

inline bool TextureStore::release(uint32_t id) {
  auto it = m_id_to_slot.find(id);
  if (it == m_id_to_slot.end()) {
    return false;  // bad / unknown / already-freed id: a no-op, never a double free
  }
  Entry& ent = m_entries[it->second];
  if (ent.level_refs > 0) {
    ent.level_refs--;
  }
  if (ent.level_refs != 0) {
    return true;  // other levels still hold it; stays live
  }
  // Last level ref gone: free the slot. The id is dead forever -- the map entry
  // is erased so no future id can ever reuse it.
  const uint32_t slot = it->second;
  m_id_to_slot.erase(it);
  ent.in_use = false;
  m_free.push_back(slot);
  m_live--;
  return true;
}

inline Entry* TextureStore::get(uint32_t id) {
  if (id == 0) {
    return nullptr;  // 0 is the invalid / failed-acquire sentinel
  }
  auto it = m_id_to_slot.find(id);
  if (it == m_id_to_slot.end()) {
    return nullptr;  // unknown, the reserved placeholder (never populated here), or freed
  }
  return &m_entries[it->second];
}

inline uint32_t TextureStore::live_count() const { return m_live; }
inline uint32_t TextureStore::high_water() const { return m_high_water; }
inline int TextureStore::failed_allocs() const { return m_failed; }

}  // namespace dktex
