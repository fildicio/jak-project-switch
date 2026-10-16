# T12 — `dk_ring.h`: per-frame ring allocator (host-testable)

**Goal:** P2.1/P2.4 groundwork. A tiny allocator that hands out aligned chunks of a big
GPU-visible buffer, split into one segment per in-flight frame. It's pure logic: no
deko3d or libnx includes, so it can be unit-tested on the Mac (T13).

**Create (1 new file):** `game/graphics/deko3d_renderer/dk_ring.h` (header-only, ~70 lines).
**Not** wrapped in `#if defined(__SWITCH__)` (host-testable).

```cpp
#pragma once
// Per-frame ring allocator for deko3d upload/UBO memory (AI-assisted). Pure logic.
#include <cstdint>

namespace dkring {

struct Alloc {
  uint8_t* cpu = nullptr;   // where to write
  uint64_t gpu = 0;         // DkGpuAddr of the same bytes
  uint32_t offset = 0;      // offset from the ring base
  bool ok() const { return cpu != nullptr; }
};

class FrameRing {
 public:
  //! base: CPU pointer of the region; gpu_base: its GPU address; size: bytes;
  //! frames: in-flight frames (2 today). size is split into `frames` equal segments,
  //! each rounded down to a multiple of 256.
  void init(uint8_t* base, uint64_t gpu_base, uint32_t size, uint32_t frames);
  //! Start writing into segment (frame_index % frames). Caller must already have waited
  //! on that frame's fence. Resets the segment's cursor.
  void begin_frame(uint64_t frame_index);
  //! align must be a power of two. Returns !ok() if the segment is full (never wraps
  //! into another frame's segment). Counts failures.
  Alloc alloc(uint32_t size, uint32_t align);
  uint32_t used() const;            // bytes used in the current segment
  uint32_t high_water() const;      // max used() ever seen (telemetry)
  uint32_t failures() const;
  uint32_t segment_size() const;
  // ... private members of your choice
};

}  // namespace dkring
```
Implement all functions inline in the header. Rules: no exceptions, no allocation,
`init` with `frames == 0` or `size < 256 * frames` leaves the ring unusable (`alloc`
returns !ok). Alignment applies to the **offset from the ring base**.

**Done-check:** host syntax check:
```sh
clang++ -std=c++17 -fsyntax-only -Wall -Wextra -x c++ game/graphics/deko3d_renderer/dk_ring.h && echo syntax-ok
```
