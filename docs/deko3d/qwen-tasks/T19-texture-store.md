# T19 — `dk_texture_pool.h`: deko3d texture store skeleton (+gtest)

**Goal:** P2.2. The bookkeeping half of `TexturePoolDeko3D`, deko3d-free and
host-testable: a fixed-capacity store of GPU texture slots with level ref-counts.
(The GPU half — `DkImage` creation, BCn direct upload, descriptor handles — is a
later task that uses this store.) Mirrors the game-facing semantics documented in
`game/graphics/texture/TexturePool.h`.

**Read first:** `TexturePool.h` lines 27–75 (the "three layers" comment: VRAM
reference → GpuTexture → data; "the same texture may appear in multiple levels,
both loaded at the same time") and `dk_buffer_pool.h` (T17) for style.

**Create (2 files + 1 edit):**

1. `game/graphics/deko3d_renderer/dk_texture_pool.h` — header-only, `namespace dktex`,
   plain C++ only (no deko3d include):

```cpp
enum class Fmt : uint8_t { Rgba8, Bc1, Bc2, Bc3, Bc7 };  // GPU format decided per texture

struct Entry {
  uint16_t w = 0, h = 0;
  uint8_t mips = 1;
  Fmt fmt = Fmt::Rgba8;
  uint32_t level_refs = 0;  // same texture loaded by N levels; free at 0 (FIX 33)
  bool in_use = false;
};

class TextureStore {
 public:
  static constexpr uint32_t kPlaceholderId = 1;  // id 1 reserved, like GL placeholder
  bool init(uint32_t capacity);   // false on capacity < 2 or OOM; id 0 never used
  uint32_t alloc(const Entry& e); // returns id, or 0 when full (caller evicts, FIX 33)
  bool add_level_ref(uint32_t id);        // false on bad id
  bool release(uint32_t id);              // --level_refs; frees slot at 0; false on bad id
  Entry* get(uint32_t id);                // nullptr on bad/free id
  uint32_t live_count() const;
  uint32_t high_water() const;
  int failed_allocs() const;
};
```

   Notes to carry as comments: slots are freed but ids are never reused (stale ids
   can't alias); `alloc` does NOT evict on its own (the loader's reclaim callback
   owns eviction — same contract as `GpuBufferPool`); BCn textures will later upload
   **direct from card bytes** with zero CPU decode (v44 fr3) — no conversion hook
   needed here; render thread only.

2. `test/test_dk_texture_pool.cpp` — gtest suite `DkTextureStore`: init reserves id 1
   (alloc returns ids ≥ 2; `get(kPlaceholderId)` non-null only after someone allocs
   it); alloc/free round-trip frees the slot (`live_count` back down); ref-counts:
   `add_level_ref` twice, first `release` keeps it alive, second frees; bad ids (0,
   unknown, double-release of a freed slot) return false and change nothing;
   capacity exhaustion returns 0 and bumps `failed_allocs`; `high_water` tracks the
   max simultaneous live count.

3. Edit `test/CMakeLists.txt`: add `test_dk_texture_pool.cpp` next to
   `test_dk_ring.cpp`. No other edits.

**Done-check** (background pattern — the host build can exceed 30 s):
```sh
nohup sh -c 'cmake --build build-host --target goalc-test -j8 && ./build-host/goalc-test --gtest_filter=DkTextureStore.*' > /tmp/dk-t19.log 2>&1 &
# repeat until it stops growing:
tail -5 /tmp/dk-t19.log
```
Expected: `[  PASSED  ]` with all DkTextureStore tests, 0 failures.

**When done:** flip your row in `docs/deko3d/qwen-tasks/README.md` to 🟡 and append a
`## T19` section to `docs/deko3d/qwen-tasks/QWEN_REPORT.md` (what you did + the
done-check last lines).
