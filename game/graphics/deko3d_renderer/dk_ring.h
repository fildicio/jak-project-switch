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

 private:
  uint8_t* base_ = nullptr;
  uint64_t gpu_base_ = 0;
  uint32_t frames_ = 0;     // in-flight frame count
  uint32_t seg_ = 0;        // current segment index (frame_ % frames_)
  uint32_t seg_size_ = 0;   // bytes per segment, a multiple of 256
  uint32_t cursor_ = 0;     // offset from ring base of the next write
  uint32_t high_water_ = 0; // max used() ever seen
  uint32_t failures_ = 0;   // count of alloc() calls that overflowed a segment
  bool usable_ = false;     // false if init() did not fit the region in the frames
};

// init: split `size` into `frames` equal segments, each rounded down to a multiple of
// 256. A ring is unusable when frames == 0 or there isn't at least one 256-byte
// segment per frame (size < 256 * frames); an unusable ring's alloc() always fails.
inline void FrameRing::init(uint8_t* base, uint64_t gpu_base, uint32_t size,
                            uint32_t frames) {
  base_ = base;
  gpu_base_ = gpu_base;
  frames_ = frames;
  seg_ = 0;
  cursor_ = 0;
  high_water_ = 0;
  failures_ = 0;
  usable_ = false;
  seg_size_ = 0;
  if (frames == 0 || size < 256u * frames) {
    return;  // not enough room for one aligned segment per frame
  }
  // size / frames is >= 256 here, so rounding down to a 256 multiple is still >= 256.
  seg_size_ = (size / frames) & ~255u;
  if (seg_size_ == 0) {
    return;  // guard: never an unaligned / zero-length segment
  }
  usable_ = true;
}

// begin_frame: move the cursor to the start of segment (frame_index % frames).
inline void FrameRing::begin_frame(uint64_t frame_index) {
  if (!usable_) {
    return;
  }
  seg_ = static_cast<uint32_t>(frame_index) % frames_;
  cursor_ = seg_ * seg_size_;
}

// alloc: hand out a power-of-two-aligned chunk within the current segment. The offset
// is aligned from the ring base. If the chunk would run past the end of this segment
// it returns a non-ok Alloc and does NOT wrap into another frame's segment.
inline Alloc FrameRing::alloc(uint32_t size, uint32_t align) {
  Alloc a;  // cpu == nullptr -> !ok()
  if (!usable_) {
    ++failures_;
    return a;
  }
  if (size > seg_size_) {
    ++failures_;  // can never fit in one segment; also prevents aligned+size wraparound
    return a;
  }
  const uint32_t seg_base = seg_ * seg_size_;
  const uint32_t seg_end = seg_base + seg_size_;
  // Round the current cursor up to the next multiple of align (offset from ring base).
  const uint32_t aligned = (cursor_ + (align - 1)) & ~static_cast<uint32_t>(align - 1);
  if (aligned + size > seg_end) {
    ++failures_;  // full: leave every other segment untouched
    return a;
  }
  cursor_ = aligned + size;
  const uint32_t used_now = cursor_ - seg_base;
  if (used_now > high_water_) {
    high_water_ = used_now;
  }
  a.offset = aligned;
  a.cpu = base_ + aligned;
  a.gpu = gpu_base_ + aligned;
  return a;
}

inline uint32_t FrameRing::used() const {
  if (!usable_) {
    return 0;
  }
  return cursor_ - (seg_ * seg_size_);
}

inline uint32_t FrameRing::high_water() const { return high_water_; }
inline uint32_t FrameRing::failures() const { return failures_; }
inline uint32_t FrameRing::segment_size() const { return seg_size_; }

}  // namespace dkring
