#pragma once
// T82 (AI-assisted): deko3d port of opengl_renderer/EyeRenderer (jak1 bucket 54,
// MERC_EYES_AFTER_PRIS). The GOAL eye code draws iris / pupil / lid sprites into a
// per-eye texture; merc then samples that texture for draws with eye_id != 0xff.
//
// capture_chain + build_frame are plain C++ (host-testable); record_eyes (dk_eye_record.cpp)
// renders every captured eye into its own 128x128 RGBA8 render target before merc.
// Render thread only.
#include <cstdint>
#include <vector>

namespace dkeye {

constexpr int kNumEyes = 40;      // GL NUM_EYE_PAIRS * 2
constexpr int kEyeTexSize = 128;  // GL GpuEyeTex FramebufferTexturePair(128, 128)
constexpr int kQuadsPerEye = 4;   // background (iris texel 0,0), iris, pupil, lid

// GL eye.vert input: xy in GS units (768 +- 256 covers the target), st in 0..1.
struct Vertex {
  float x, y, s, t;
};

struct Eye {
  uint8_t slot = 0;           // GL tex_slot() = pair * 2 + lr
  uint32_t iris = 0;          // dktexgpu ids, 0 = missing (that quad is skipped, like GL)
  uint32_t pupil = 0;
  uint32_t lid = 0;
  Vertex v[kQuadsPerEye][4];  // triangle strips
};

struct CaptureStats {
  uint32_t eyes = 0;
  uint32_t bad = 0;     // unexpected DMA shape: the rest of the bucket is dropped
  uint32_t no_tex = 0;  // iris/pupil/lid textures not in the dk pool
};

using LookupFn = uint32_t (*)(uint32_t tbp);  // tbp -> dktexgpu id, 0 = missing

// Render thread, before the chain is acked. data = EE memory base (chain base).
void capture_chain(const void* data, uint32_t start_offset, LookupFn lookup);
const CaptureStats& capture_stats();
const std::vector<Eye>& captured_eyes();

struct FrameDraws {
  std::vector<Eye> eyes;  // copy of the captured eyes (record reads the ids/slots)
  uint64_t vert_gpu = 0;  // all eyes' vertices, eye i quad q at (i * 4 + q) * 4
  uint32_t vert_bytes = 0;
  uint32_t dropped = 0;   // GPU ring full
};
void build_frame();
const FrameDraws& frame_draws();

// Merc: the dktexgpu id of eye eye_id (GL lookup_eye_texture: eye_id % 40), or 0 if that eye
// has never been drawn. Set by record_eyes.
uint32_t eye_tex_id(uint8_t eye_id);
void set_eye_tex_id(int slot, uint32_t id);

// Switch only (dk_eye_record.cpp): draws frame_draws() into the eye targets, then rebinds the
// frame target. Call inside the world pass, before merc.
void record_eyes();

}  // namespace dkeye
