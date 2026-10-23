/*!
 * dk_ocean_texture.h - deko3d ocean texture generator, CPU half (T95, AI-assisted)
 *
 * Verbatim copy of the jak1-relevant half of
 * game/graphics/opengl_renderer/ocean/OceanTexture.h + OceanTexture_PC.cpp
 * (the "PC" VU1 emulation that handle_ocean_texture_jak1 drives:
 * run_L1_PC / run_L2_PC / run_L3_PC / run_L5_PC / xgkick_PC).
 *
 * Differences from the GL header (and the ONLY differences):
 *  - class renamed OceanTexture -> dkocean::TextureGen (no base class; the
 *    GL BucketRenderer base is dropped)
 *  - opengl_utils.h (FramebufferTexturePair) and the two GL FBO members
 *    (m_result_texture / m_temp_texture) are dropped: the deko3d half draws
 *    into a dktexgpu render target instead (dk_ocean_flush.cpp)
 *  - generate_mipmaps is dropped: dktexgpu textures are single-level, so the
 *    GL mip chain (make_texture_with_mipmaps + OCEAN_TEXTURE_MIPMAP shader)
 *    has no dk equivalent in this pass (documented T95 limitation)
 *  - the GL m_pc handle fields (vao/buffers) are dropped; the vectors stay
 *    and init() fills the static positions + restart index list (port of
 *    init_pc's loop)
 *  - the GL init_textures/draw_debug_window/dtor and the jak2-only
 *    run_*_PC_jak2 functions are dropped (jak1 scope, T95)
 *  - handle_ocean_texture_jak1 keeps its exact DMA walk; the GL calls at the
 *    end (flush / mipmap chain / move_existing_to_vram) become the
 *    output_ready() flag + stats (the recorder reads the arrays)
 *
 * Plain C++ header: no deko3d includes here (see dk_device.h comment).
 */

#pragma once

#include "common/common_types.h"
#include "common/dma/dma_chain_read.h"
#include "common/math/Vector.h"
#include "common/util/Assert.h"
#include "game/common/vu.h"
#include "game/graphics/opengl_renderer/AdgifHandler.h"

#include <vector>

namespace dkocean {

class TextureGen {
 public:
  TextureGen() = default;

  // Fills the static positions + restart index list (port of GL init_pc's
  // loop). Call once before the first handle_ocean_texture_jak1.
  void init();

  // Port of GL handle_ocean_texture_jak1: walks the DMA and runs the VU1
  // program. Pure CPU; the draw itself is recorded by dk_ocean_flush.cpp.
  void handle_ocean_texture_jak1(DmaFollower& dma);

  // ---- output for the deko3d recorder (GL flush() read these instead) ----
  bool output_valid() const { return m_pc.vtx_idx == NUM_VERTS; }
  const std::vector<math::Vector2f>& positions() const { return m_pc.vertex_positions; }
  // GL OceanTexture::Vertex (public here: the recorder uploads it verbatim).
  struct VertexDynamic {
    float s, t;
    math::Vector<u8, 4> rgba;
    u32 pad;
  };
  static_assert(sizeof(VertexDynamic) == 16);
  const std::vector<VertexDynamic>& dynamic_vertices() const { return m_pc.vertex_dynamic; }
  const std::vector<u32>& indices() const { return m_pc.index_buffer; }
  // Raw GsTex0 data of the envmap texture the wave draw samples.
  u64 envmap_tex0_data() const { return m_envmap_adgif.tex0_data; }

  struct Stats {
    u32 frames = 0;   // completed walks
    u32 aborts = 0;   // walks that failed an assert (skipped, counted)
  };
  const Stats& stats() const { return m_stats; }

  // T99: last-completed-walk probe data (constants snapshot + output range),
  // read by the periodic console probe in dk_ocean_flush.cpp. walks == 0 means
  // no walk ever completed (the arrays below are then meaningless).
  struct WalkProbe {
    u32 walks = 0;      // m_stats.frames right after that walk
    float s_min = 0.f, s_max = 0.f;
    float t_min = 0.f, t_max = 0.f;
    u32 s_nonzero = 0;  // verts with |s| > 0.001 (2112 = every vert has real UVs)
    u8 r_max = 0, g_max = 0, b_max = 0, a_max = 0;
    float start_x = 0.f, offsets_x = 0.f, cam_nrm_x = 0.f;  // VU constants input
  };
  const WalkProbe& walk_probe() const { return m_walk_probe; }

 private:
  void run_L1_PC();
  void run_L2_PC();
  void run_L3_PC();
  void run_L5_PC();
  void xgkick_PC(Vf* src);

  void setup_renderer();

  // (deftype ocean-texture-constants (structure)
  struct OceanTextureConstants {
    //  ((giftag    qword    :inline :offset-assert 0) 985
    u8 giftag[16];
    //   (buffers   vector4w :inline :offset-assert 16) 986
    math::Vector<u32, 4> buffers;
    //   (dests     vector4w :inline :offset-assert 32) 987
    math::Vector<u32, 4> dests;
    //   (start     vector   :inline :offset-assert 48) 988
    math::Vector4f start;
    //   (offsets   vector   :inline :offset-assert 64) 989
    math::Vector4f offsets;
    //   (constants vector   :inline :offset-assert 80) 990
    math::Vector4f constants;
    //   (cam-nrm   vector   :inline :offset-assert 96) 991
    math::Vector4f cam_nrm;
    //   )
  } m_texture_constants;
  static_assert(sizeof(OceanTextureConstants) == 112);

  AdGifData m_envmap_adgif;

  Vf m_texture_vertices_a[192];
  Vf m_texture_vertices_b[192];

  static constexpr int DBUF_SIZE = 99;
  Vf m_dbuf_a[DBUF_SIZE];
  Vf m_dbuf_b[DBUF_SIZE];

  Vf* m_dbuf_x;
  Vf* m_dbuf_y;

  static constexpr int TBUF_SIZE = 199;
  Vf m_tbuf_a[TBUF_SIZE];
  Vf m_tbuf_b[TBUF_SIZE];

  Vf* m_tbuf_x;
  Vf* m_tbuf_y;

  Vf* m_texture_vertices_loading = nullptr;
  Vf* m_texture_vertices_drawing = nullptr;

  Vf* swap_vu_upload_buffers() {
    std::swap(m_texture_vertices_drawing, m_texture_vertices_loading);
    return m_texture_vertices_drawing;
  }

  void swap_dbuf() { std::swap(m_dbuf_x, m_dbuf_y); }

  void swap_tbuf() { std::swap(m_tbuf_x, m_tbuf_y); }

  Vf* get_dbuf() { return m_dbuf_x; }

  Vf* get_dbuf_other() { return m_dbuf_y; }

  Vf* get_tbuf() { return m_tbuf_x; }

  struct {
    Vf startx;  //           vf14
    // Vf base_pos;          vf15
    // Vf nrm0;              vf24
    Vf* dbuf_read_a;      // vi03
    Vf* dbuf_read_b;      // vi04
    Vf* in_ptr;           // vi05
    Vf* dbuf_write;       // vi06
    Vf* dbuf_write_base;  // vi07
    Vf* tptr;             // vi08
    Vf* tbase;            // vi09
  } vu;

  static constexpr u32 NUM_STRIPS = 32;
  static constexpr u32 NUM_VERTS_PER_STRIP = 66;
  static constexpr u32 NUM_VERTS = NUM_STRIPS * NUM_VERTS_PER_STRIP;

  // note: if we used u16's for s/t, we could make this 8 bytes, but I'm afraid that some GPUs
  // will be unhappy with that format.
  // (T95) struct VertexDynamic moved to the public section above.
  struct {
    std::vector<math::Vector2f> vertex_positions;
    std::vector<VertexDynamic> vertex_dynamic;
    std::vector<u32> index_buffer;
    u32 vtx_idx = 0;
  } m_pc;

  enum TexVu1Data {
    BUF0 = 384,
    BUF1 = 583,
    DEST0 = 782,
    DEST1 = 881,
    CONSTANTS = 985,
  };

  enum TexVu1Prog { START = 0, REST = 2, DONE = 4 };

  static constexpr int NUM_FRAG_LOOPS = 9;

  Stats m_stats;
  WalkProbe m_walk_probe;
};

}  // namespace dkocean
