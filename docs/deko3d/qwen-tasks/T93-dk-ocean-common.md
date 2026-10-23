# T93 — Ocean dk common recorder (verbatim CPU decode copy)

**Depends on:** T91a/b/c (survey, ✅). **Category:** D ocean. **Size:** small-medium (1 short session).
**Law:** zero design freedom. Header + test are pasted verbatim below; the `.cpp` is a
function-by-function verbatim copy with a rename table. Anything not in the table → STOP,
paste the problem in the report, change nothing else.

## Goal
First code task of the ocean port. The GL `CommonOceanRenderer` decode half
(GIF packet → vertex/index data) is pure CPU (T91b, zero GL calls between ctor and flush).
Copy it into a dk recorder with the GL **constructor / destructor / `flush_near` /
`flush_mid` removed**. This class only ACCUMULATES data; the dk draw recording (the flush
equivalent: depth ALWAYS, reverse-index envmap trick, `DST_ALPHA`/`ONE` blend) is a later
reviewer task that reads the accumulated data.

## Create 1 — `game/graphics/deko3d_renderer/dk_ocean_common.h` (paste verbatim)

```cpp
#pragma once
// T93 (AI-assisted): deko3d ocean port, shared CPU recorder. Verbatim copy of the GL
// CommonOceanRenderer decode path (init/kick/handle_* functions) with the GL
// ctor/dtor/flush_* removed: this class only ACCUMULATES the same 32-byte vertices and
// the 3 restart-primitive index buckets. The dk draw recording (flush equivalent) is a
// later task and reads this data between init_for_*() and the next init.
// Plain C++ header (no deko3d types).
#include <cstdint>
#include <vector>

#include "common/math/Vector.h"

namespace dkocean {

// Bit-identical to CommonOceanRenderer::Vertex (static_assert is the proof).
struct Vertex {
  math::Vector<float, 3> xyz;
  math::Vector<u8, 4> rgba;
  math::Vector<float, 3> stq;
  u8 fog;
  u8 pad[3];
};
static_assert(sizeof(Vertex) == 32);

// GL flush_mid/flush_near call this on the envmap bucket; the dk flush does the same.
void reverse_indices(u32* indices, u32 count);

class CommonRecorder {
 public:
  CommonRecorder();

  void init_for_near();
  void kick_from_near(const u8* data);
  void init_for_mid();
  void kick_from_mid(const u8* data);

  // --- recorded output, read by the dk record layer before the next init ---
  const Vertex* vertices() const { return m_vertices.data(); }
  u32 vertex_count() const { return m_next_free_vertex; }
  const u32* indices(int bucket) const { return m_indices[bucket].data(); }
  u32 index_count(int bucket) const { return m_next_free_index[bucket]; }
  // tbp of the most recent envmap adgif (0 before the first one).
  u32 envmap_tex() const { return m_envmap_tex; }

 private:
  void handle_near_vertex_gif_data_fan(const u8* data, u32 offset, u32 loop);
  void handle_near_vertex_gif_data_strip(const u8* data, u32 offset, u32 loop);
  void handle_near_adgif(const u8* data, u32 offset, u32 count);
  void handle_mid_adgif(const u8* data, u32 offset);

  enum VertexBucket {
    RGB_TEXTURE = 0,
    ALPHA = 1,
    ENV_MAP = 2,
  };
  u32 m_current_bucket = VertexBucket::RGB_TEXTURE;

  static constexpr int NUM_BUCKETS = 3;

  std::vector<Vertex> m_vertices;
  u32 m_next_free_vertex = 0;

  std::vector<u32> m_indices[NUM_BUCKETS];
  u32 m_next_free_index[NUM_BUCKETS] = {0};

  u32 m_envmap_tex = 0;
};

}  // namespace dkocean
```

## Create 2 — `game/graphics/deko3d_renderer/dk_ocean_common.cpp` (copy + rename table)

Source: `game/graphics/opengl_renderer/ocean/CommonOceanRenderer.cpp`.
Copy these functions **verbatim, in file order, comment lines included**:

- `init_for_near`
- `kick_from_near`
- `handle_near_vertex_gif_data_fan`
- `handle_near_vertex_gif_data_strip`
- `handle_near_adgif`
- `init_for_mid`
- `kick_from_mid`
- `handle_mid_adgif`
- the free function `reverse_indices`

Rename table (the ONLY changes allowed):

| old | new |
|---|---|
| `#include "CommonOceanRenderer.h"` and the GfxDrawStats/log includes | the skeleton below |
| `CommonOceanRenderer::` | `CommonRecorder::` |

Do **NOT** copy: constructor (lines 9–71), destructor (73–77), `flush_near`, `flush_mid`,
the `OCEAN_TEX_TBP_*` constants, `#include "game/graphics/opengl_renderer/GfxDrawStats.h"`.

File skeleton (paste the copied functions where marked, `reverse_indices` last):

```cpp
// T93 (AI-assisted): see dk_ocean_common.h. Decode functions copied verbatim from
// game/graphics/opengl_renderer/ocean/CommonOceanRenderer.cpp (rename table in the task
// file); the GL ctor/dtor/flush_* are NOT copied.
#include "game/graphics/deko3d_renderer/dk_ocean_common.h"

#if defined(__SWITCH__) || defined(DK_HOST_REPLAY)

#include <cstring>

#include "common/dma/dma_chain_read.h"
#include "common/dma/gs.h"
#include "common/util/Assert.h"
#include "game/graphics/opengl_renderer/AdgifHandler.h"

namespace dkocean {

CommonRecorder::CommonRecorder() {
  m_vertices.resize(4096 * 10);
  for (auto& buf : m_indices) {
    buf.resize(4096 * 10);
  }
}

// ---- copied functions here ----

}  // namespace dkocean
#endif
```

## Create 3 — `test/test_dk_ocean_common.cpp` (paste verbatim)

```cpp
// T93 (AI-assisted): host tests for the dk ocean common recorder. The decode functions
// are a verbatim copy of the GL CommonOceanRenderer ones and are validated end-to-end by
// the later ocean replay task; here we pin the vertex layout (must match the GL struct
// the VU math fills, 32 bytes) and the two pure helpers the dk flush relies on.

#include <cstddef>
#include <cstdint>

#include "game/graphics/deko3d_renderer/dk_ocean_common.h"
#include "gtest/gtest.h"

TEST(OceanDkCommon, VertexLayout) {
  ASSERT_EQ(sizeof(dkocean::Vertex), 32u);
  EXPECT_EQ(offsetof(dkocean::Vertex, xyz), 0u);
  EXPECT_EQ(offsetof(dkocean::Vertex, rgba), 12u);
  EXPECT_EQ(offsetof(dkocean::Vertex, stq), 16u);
  EXPECT_EQ(offsetof(dkocean::Vertex, fog), 28u);
}

TEST(OceanDkCommon, InitResets) {
  dkocean::CommonRecorder r;
  r.init_for_near();
  EXPECT_EQ(r.vertex_count(), 0u);
  EXPECT_EQ(r.index_count(0), 0u);
  EXPECT_EQ(r.index_count(1), 0u);
  EXPECT_EQ(r.index_count(2), 0u);
  r.init_for_mid();
  EXPECT_EQ(r.vertex_count(), 0u);
}

TEST(OceanDkCommon, ReverseIndices) {
  u32 x[5] = {0, UINT32_MAX, 2, 3, 4};
  dkocean::reverse_indices(x, 5);
  // plain swap reversal (restart markers are just values)
  EXPECT_EQ(x[0], 4u);
  EXPECT_EQ(x[1], 3u);
  EXPECT_EQ(x[2], 2u);
  EXPECT_EQ(x[3], UINT32_MAX);
  EXPECT_EQ(x[4], 0u);
}
```

## Edit (2 lines)

1. `game/CMakeLists.txt` — add `graphics/deko3d_renderer/dk_ocean_common.cpp` directly
   under the `graphics/deko3d_renderer/SkyDk.cpp` line (~line 114).
2. `test/CMakeLists.txt` — add `${CMAKE_CURRENT_LIST_DIR}/test_dk_ocean_common.cpp` to
   the `goalc-test` `add_executable` list, next to `test_dk_buffer_pool.cpp` (~line 21).
   (`DK_HOST_REPLAY=1` is already defined for that target at line 65, so the guarded
   body compiles on host.)

## Done-check (all must pass)

```
python3 scripts/ai/check-dk.py
cmake --build build-host --target goalc-test -j8 2>&1 | tail -3 && \
  ./build-host/goalc-test --gtest_filter='OceanDkCommon.*' 2>&1 | tail -4
grep -n "gl[A-Z]" game/graphics/deko3d_renderer/dk_ocean_common.h game/graphics/deko3d_renderer/dk_ocean_common.cpp ; echo "gl-grep exit: $?"
```

- dk-lint: PASS
- gtest: `[  PASSED  ] 3 tests`
- gl-grep: no matches (exit 1)

## Report (QWEN_REPORT.md §T93)

1. The three done-check outputs.
2. The FULL output of
   `diff -u game/graphics/opengl_renderer/ocean/CommonOceanRenderer.cpp game/graphics/deko3d_renderer/dk_ocean_common.cpp`
   — paste it whole; the reviewer verifies the copy is verbatim (expected diff: dropped
   includes/ctor/dtor/flush functions, `CommonOceanRenderer::` → `CommonRecorder::`,
   namespace wrapper, plus the GL file's flush bodies that were not copied).
