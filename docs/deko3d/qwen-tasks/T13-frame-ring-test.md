# T13 — gtest for `dk_ring.h`

**Goal:** prove the T12 allocator works, on the Mac, before it ever runs on a Switch.

**Create (1 file):** `test/test_dk_ring.cpp`.
**Edit (1 line):** `test/CMakeLists.txt` — add the file to the `add_executable(goalc-test ...)`
source list, same style as the other entries.

**Tests (one `TEST(DkRing, ...)` each):**
1. `AlignsOffsets` — alloc(10, 256) twice → offsets 0 and 256; `gpu == gpu_base + offset`.
2. `SegmentsPerFrame` — frames=2: frame 0 and frame 1 allocations land in different halves.
3. `BeginFrameResets` — after begin_frame(2) (same segment as 0), first alloc offset = segment start.
4. `FullSegmentFails` — allocate past segment size → `!ok()`, `failures() == 1`, and
   the next frame's segment is untouched.
5. `HighWaterTracks` — high_water() keeps the max across frames.
6. `BadInitUnusable` — frames=0 → alloc returns !ok.

Use a `std::vector<uint8_t>` as the "memory" and a fake gpu_base like `0x100000000`.
Include with `#include "game/graphics/deko3d_renderer/dk_ring.h"` and `#include "gtest/gtest.h"`.

**Done-check:**
```sh
cmake --build build-host --target goalc-test -j8 2>&1 | tail -3 && \
  ./build-host/goalc-test --gtest_filter='DkRing.*' 2>&1 | tail -4
```
Expected: `[  PASSED  ] 6 tests.`
