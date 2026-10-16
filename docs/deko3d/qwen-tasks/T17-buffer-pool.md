# T17 — `dk_buffer_pool.h`: FIX 33 pooling on a fixed deko3d region (+gtest)

**Goal:** P2.1. Port the *semantics* of `game/graphics/opengl_renderer/loader/GpuBufferPool.h`
(FIX 33: pooled buffers so area transitions stop re-allocating GPU memory) to deko3d,
where memory is one fixed region we own (a slice of a `DkMemBlock` — wiring comes in a
later task). Pure logic, no deko3d include, host-testable like `dk_ring.h`.

**Read first:** `GpuBufferPool.h` (whole file — especially the block comments) and
`game/graphics/deko3d_renderer/dk_ring.h` (style reference: namespace, header-only,
plain C++). Also `docs/deko3d/qwen-tasks/T13-dk-ring-test.md` for the gtest pattern.

**Create (2 files + 1 edit):**

1. `game/graphics/deko3d_renderer/dk_buffer_pool.h` — header-only, `namespace dkpool`,
   `#include <cstdint> <functional> <vector> <unordered_map>`:

```cpp
struct PoolBuffer {
  uint32_t id = 0;          // 0 = invalid (failed acquire)
  uint8_t* cpu = nullptr;   // CPU address of the chunk
  uint64_t gpu = 0;         // GPU address of the same bytes
  uint32_t capacity = 0;    // bytes actually reserved (the size class)
  bool ok() const { return cpu != nullptr; }
};

class BufferPool {
 public:
  static constexpr uint32_t SIZE_CLASS = 256 * 1024;  // same classes as GpuBufferPool
  void init(uint8_t* base, uint64_t gpu_base, uint32_t size);  // fixed region; never grows
  PoolBuffer acquire(uint32_t size);
  void release(uint32_t id);
  void set_reclaim_callback(std::function<bool()> cb) { m_reclaim = std::move(cb); }
  // telemetry
  uint32_t outstanding() const;
  uint32_t pooled_chunks() const;
  size_t pooled_bytes() const;
  uint32_t region_unused_bytes() const;
  int failed_allocations() const;
};
```

2. Semantics to port **exactly** from `GpuBufferPool.h` (write each as a comment):
   - `size_class_of`: round up to `SIZE_CLASS` multiples, never zero.
   - acquire: best fit = smallest **free** size class ≥ want; refuse an oversized
     reuse when `best > want * 2` (want = the size class, not the raw size).
   - Nothing suitable pooled → carve a new chunk from the bump pointer
     (new region, no driver). Region exhausted → run the reclaim callback and retry
     (pool first, then bump) until it returns false → `failed_allocations++`,
     return `!ok()`. **Caller contract (FIX 33): on `!ok()` the caller must SKIP the
     upload, never write through a null cpu.**
   - `release`: ignore id 0, unknown ids and double-releases (defensive against the
     old hfrag double-delete bug class). Ids are never reused (monotonic counter), so
     a stale id can never alias a live chunk.
   - Reused chunks keep old bytes: caller always rewrites the whole logical range
     before anything draws from it (same argument as the GL version).
   - Not thread-safe: render thread only (every loader GPU stage runs there).

3. `test/test_dk_buffer_pool.cpp` — gtest, `#include "game/graphics/deko3d_renderer/dk_buffer_pool.h"`,
   suite name `DkBufferPool`. Cover at least: carve on first acquire; release→acquire
   **reuses the same cpu address**; the ≤2× rule (free a 1 MB-class chunk by acquiring
   800 KB, then acquire 100 KB → must CARVE, not reuse the 1 MB chunk); bad releases
   ignored (0, unknown id, double release); region exhaustion → reclaim callback that
   releases the outstanding buffer → retry succeeds; reclaim returns false →
   `failed_allocations()` becomes 1 and acquire fails; telemetry numbers sane.

4. Edit `test/CMakeLists.txt`: add `test_dk_buffer_pool.cpp` next to `test_dk_ring.cpp`
   in the `goalc-test` sources. No other edits.

**Done-check** (host build can exceed 30 s — use the background pattern, no foreground
`cmake --build`):
```sh
nohup sh -c 'cmake --build build-host --target goalc-test -j8 && ./build-host/goalc-test --gtest_filter=DkBufferPool.*' > /tmp/dk-t17.log 2>&1 &
# then repeat until it stops growing:
tail -5 /tmp/dk-t17.log
```
Expected: `[  PASSED  ]` with all DkBufferPool tests, 0 failures.

**When done:** flip your row in `docs/deko3d/qwen-tasks/README.md` to 🟡 and append a
`## T17` section to `docs/deko3d/qwen-tasks/QWEN_REPORT.md` (what you did + the last
lines of the done-check).
