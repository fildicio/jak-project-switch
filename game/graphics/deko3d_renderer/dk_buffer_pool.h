#pragma once
// deko3d buffer pool (AI-assisted, T17). FIX 33 ported from
// game/graphics/opengl_renderer/loader/GpuBufferPool.h to deko3d, where GPU
// memory is ONE fixed region we own (a slice of a DkMemBlock; the wiring into a
// real DkMemBlock is a later task). Pure logic, host-testable like dk_ring.h.
//
// The GL version's churn (glGenBuffers + glBufferData on every load, dying inside
// the Tegra suballocator on area transitions) is removed by pooling: acquire()
// reuses a previously released chunk of a close-enough size class instead of
// carving new storage; release() recycles the chunk back into the pool. Once
// warmed, steady-state GPU use tracks "live chunks" not "chunks ever visited".
//
// Not thread-safe: render thread only (every loader GPU stage runs there).
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace dkpool {

// One handed-out or handed-back chunk, returned by acquire().
struct PoolBuffer {
  uint32_t id = 0;          // 0 = invalid (a failed acquire returns id 0 / !ok())
  uint8_t* cpu = nullptr;   // CPU address of the chunk (write the logical range here)
  uint64_t gpu = 0;         // GPU address of the same bytes
  uint32_t capacity = 0;    // bytes actually reserved (the size class)
  bool ok() const { return cpu != nullptr; }
};

class BufferPool {
 public:
  static constexpr uint32_t SIZE_CLASS = 256 * 1024;  // same classes as GpuBufferPool

  // Init with a fixed region: base is the CPU address, gpu_base the GPU address
  // of the same bytes, size the region length in bytes. Never grows, never
  // shrinks. Resets all counters; does NOT touch a previously set reclaim cb.
  void init(uint8_t* base, uint64_t gpu_base, uint32_t size);

  // Hand out a chunk with capacity >= `size` bytes (capacity is always a whole
  // size class). Best fit = smallest FREE size class >= want; refuse an oversized
  // reuse when best > want * 2 (want is the size class, not the raw size). Nothing
  // suitable pooled -> carve a new chunk from the bump pointer. Region exhausted ->
  // run the reclaim callback and retry (pool first, then bump) until it returns
  // false -> failed_allocations++ and return !ok().
  //
  // Caller contract (FIX 33): on !ok() the caller MUST skip the upload, never write
  // through a null cpu (a failed chunk keeps no backing storage).
  PoolBuffer acquire(uint32_t size);

  // Recycle a chunk previously handed out. Id 0, unknown ids and double-releases
  // are ignored (defensive against the old hfrag double-delete bug class). Ids are
  // never reused (monotonic counter), so a stale id can never alias a live chunk.
  // Issues no driver calls.
  void release(uint32_t id);

  // Reclaim callback: free one chunk of reclaimable memory per call and return true
  // if it actually freed something; acquire() keeps calling it (and retrying) until
  // it returns false. Render thread only.
  void set_reclaim_callback(std::function<bool()> cb) { m_reclaim = std::move(cb); }

  // Telemetry.
  uint32_t outstanding() const;     // live (handed-out) chunk count
  uint32_t pooled_chunks() const;   // free (pooled) chunk count
  size_t pooled_bytes() const;      // bytes inside the free list (capacity sum)
  uint32_t region_unused_bytes() const;  // bytes of the region never carved (size - bump)
  int failed_allocations() const;

 private:
  // A single carved chunk's bookkeeping.
  struct Chunk {
    uint32_t offset = 0;   // offset from the region base
    uint32_t capacity = 0;  // size class in bytes
  };

  static uint32_t size_class_of(uint32_t size) {
    // Round up to a multiple of SIZE_CLASS, never zero (no zero-sized classes).
    const uint32_t cls = ((size + SIZE_CLASS - 1) / SIZE_CLASS) * SIZE_CLASS;
    return cls < SIZE_CLASS ? SIZE_CLASS : cls;
  }

  // Smallest free size class >= want, or 0 if none.
  uint32_t best_fit(uint32_t want) const {
    uint32_t best = 0;
    for (const auto& [cls, list] : m_free) {
      if (!list.empty() && cls >= want && (best == 0 || cls < best)) {
        best = cls;
      }
    }
    // Don't waste more than 2x the request when reusing an oversized chunk.
    if (best != 0 && static_cast<uint64_t>(best) > static_cast<uint64_t>(want) * 2) {
      best = 0;
    }
    return best;
  }

  uint8_t* base_ = nullptr;
  uint64_t gpu_base_ = 0;
  uint32_t size_ = 0;             // region length in bytes
  uint32_t bump_ = 0;             // offset from base of the next carve (never moves down)
  // outstanding id -> chunk
  std::unordered_map<uint32_t, Chunk> m_live;
  // size class -> free chunk offsets
  std::unordered_map<uint32_t, std::vector<uint32_t>> m_free;
  size_t m_pooled_bytes = 0;      // capacity sum of free chunks
  uint32_t m_next_id = 1;         // ids are monotonic and never reused (0 reserved)
  int m_failed_allocations = 0;
  std::function<bool()> m_reclaim;
};

// A fresh pool carves nothing: bump at 0, no live/pooled chunks, id counter at 1.
inline void BufferPool::init(uint8_t* base, uint64_t gpu_base, uint32_t size) {
  base_ = base;
  gpu_base_ = gpu_base;
  size_ = size;
  bump_ = 0;
  m_live.clear();
  m_free.clear();
  m_pooled_bytes = 0;
  m_next_id = 1;
  m_failed_allocations = 0;
}

// Pool (best fit, <= 2x) -> carve from bump -> reclaim-and-retry loop.
inline PoolBuffer BufferPool::acquire(uint32_t size) {
  const uint32_t want = size_class_of(size);
  while (true) {
    // 1. Reuse a pooled chunk of a close-enough size class.
    const uint32_t best = best_fit(want);
    if (best != 0) {
      auto& list = m_free[best];
      const uint32_t offset = list.back();
      list.pop_back();
      if (list.empty()) {
        m_free.erase(best);
      }
      m_pooled_bytes -= best;
      const uint32_t id = m_next_id++;
      m_live[id] = Chunk{offset, best};
      PoolBuffer pb;
      pb.id = id;
      pb.cpu = base_ + offset;
      pb.gpu = gpu_base_ + offset;
      pb.capacity = best;
      return pb;
    }
    // 2. Nothing pooled fits -> carve a new chunk from the bump pointer (region,
    //    no driver). A carve overflows the region when bump + want would pass size_.
    if (static_cast<uint64_t>(bump_) + want <= size_) {
      const uint32_t offset = bump_;
      bump_ += want;
      const uint32_t id = m_next_id++;
      m_live[id] = Chunk{offset, want};
      PoolBuffer pb;
      pb.id = id;
      pb.cpu = base_ + offset;
      pb.gpu = gpu_base_ + offset;
      pb.capacity = want;
      return pb;
    }
    // 3. Region exhausted -> run the reclaim callback and retry (pool first, then
    //    bump). Stop (and count a failure) when there is no callback or it frees
    //    nothing, so the loop always terminates (a finite number of live chunks).
    if (!m_reclaim || !m_reclaim()) {
      m_failed_allocations++;
      return PoolBuffer{};  // !ok() : caller must skip the upload
    }
  }
}

inline void BufferPool::release(uint32_t id) {
  if (id == 0) {
    return;
  }
  auto it = m_live.find(id);
  if (it == m_live.end()) {
    return;  // unknown / already-released id: ignored, never reuses
  }
  const Chunk c = it->second;
  m_live.erase(it);
  m_free[c.capacity].push_back(c.offset);
  m_pooled_bytes += c.capacity;
}

inline uint32_t BufferPool::outstanding() const { return static_cast<uint32_t>(m_live.size()); }

inline uint32_t BufferPool::pooled_chunks() const {
  uint32_t n = 0;
  for (const auto& [cls, list] : m_free) {
    n += static_cast<uint32_t>(list.size());
  }
  return n;
}

inline size_t BufferPool::pooled_bytes() const { return m_pooled_bytes; }

inline uint32_t BufferPool::region_unused_bytes() const {
  // Bytes of the region never carved. bump_ never exceeds size_ (carves only
  // land when they fit), so this subtraction is well-defined.
  return bump_ >= size_ ? 0u : (size_ - bump_);
}

inline int BufferPool::failed_allocations() const { return m_failed_allocations; }

}  // namespace dkpool

