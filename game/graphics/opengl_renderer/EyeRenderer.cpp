#include "EyeRenderer.h"

#include "game/graphics/opengl_renderer/background/background_common.h"
#include "game/graphics/opengl_renderer/GfxDrawStats.h"

#include "common/util/FileUtil.h"

#include "game/graphics/opengl_renderer/AdgifHandler.h"

#include <atomic>
#include <chrono>

#if defined(__SWITCH__)
#include "game/switch/imgui_stub.h"
#else
#include "third-party/imgui/imgui.h"
#endif

/////////////////////////
// Bucket Renderer
/////////////////////////
namespace {
// FIX 106 (AI-assisted): tex_slot() is pair*2+lr, where pair is derived from the eye
// draw's screen-space Y in get_draws(). An out-of-range pair would index past
// m_gpu_eye_textures (40 entries) -- an out-of-bounds write on the render thread.
// Returns the slot, or -1 to skip the draw.
int safe_eye_slot(int slot) {
  return (slot >= 0 && slot < NUM_EYE_PAIRS * 2) ? slot : -1;
}

// FIX 106 (AI-assisted): the old prints fired once per failing draw -- at 30 fps in
// a stadium crowd that is thousands of SD-card writes per minute (switch_safe_stdout
// was on the f103c 20:02 crash stack). Print the first failure, then at most one
// summary line every 30 s.
void log_eye_lookup_failed(const char* why, u64 key) {
  static std::atomic<u64> s_total{0};
  static std::atomic<u64> s_last_log_ms{0};
  const u64 total = s_total.fetch_add(1) + 1;
  const u64 now_ms = (u64)std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
  const u64 last = s_last_log_ms.load(std::memory_order_relaxed);
  if (total == 1) {
    fmt::print("lookup eye failed for {} {} [first]\n", key, why);
  } else if ((now_ms - last) > 30000 && s_last_log_ms.compare_exchange_weak(last, now_ms)) {
    fmt::print("lookup eye failed for {} {} [total {}]\n", key, why, total);
  }
}
}  // namespace

EyeRenderer::EyeRenderer(const std::string& name, int id) : BucketRenderer(name, id) {}

void EyeRenderer::init_textures(TexturePool& texture_pool, GameVersion version) {
  // set up eyes
  for (int pair_idx = 0; pair_idx < NUM_EYE_PAIRS; pair_idx++) {
    for (int lr = 0; lr < 2; lr++) {
      u32 tidx = pair_idx * 2 + lr;

      u32 tbp = pair_idx * 2 + lr;
      switch (version) {
        case GameVersion::Jak1:
          tbp += EYE_BASE_BLOCK_JAK1;
          break;
        case GameVersion::Jak2:
          // NOTE: using jak 1's address because jak 2's breaks some ocean stuff.
          // this is a little suspicious, I think we're possibly just getting lucky here.
          tbp += EYE_BASE_BLOCK_JAK1;
          break;
        case GameVersion::Jak3:
        case GameVersion::JakX:
          // for jak 3, go back to using the right TBP.
          tbp += EYE_BASE_BLOCK_JAK3;
          break;
        default:
          ASSERT_NOT_REACHED();
      }
      TextureInput in;
      in.gpu_texture = m_gpu_eye_textures[tidx].fb.texture();
      in.w = 32;
      in.h = 32;
      in.debug_page_name = "PC-EYES";
      in.debug_name = fmt::format("{}-eye-gpu-{}", lr ? "left" : "right", pair_idx);
      in.id = texture_pool.allocate_pc_port_texture(version);
      m_gpu_eye_textures[tidx].gpu_tex = texture_pool.give_texture_and_load_to_vram(in, tbp);
      m_gpu_eye_textures[tidx].tbp = tbp;
      // FIX 106 (AI-assisted): remember our own stable GL name for this slot (it is
      // the same name the pool wrapped above and never changes) so lookups never
      // have to touch the pool's GpuTexture again.
      m_gpu_eye_textures[tidx].gl_tex = in.gpu_texture;
    }
  }

  // set up vertices for GPU mode
  glGenVertexArrays(1, &m_vao);
  glBindVertexArray(m_vao);
  glGenBuffers(1, &m_gl_vertex_buffer);
  glBindBuffer(GL_ARRAY_BUFFER, m_gl_vertex_buffer);
  glBufferData(GL_ARRAY_BUFFER, VTX_BUFFER_FLOATS * sizeof(float), nullptr, GL_STREAM_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0,                  // location 0 in the shader
                        4,                  // 2 floats per vert
                        GL_FLOAT,           // floats
                        GL_TRUE,            // normalized, ignored,
                        sizeof(float) * 4,  //
                        (void*)0            // offset in array
  );
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindVertexArray(0);
}

EyeRenderer::~EyeRenderer() {
  glDeleteVertexArrays(1, &m_vao);
  glDeleteBuffers(1, &m_gl_vertex_buffer);
}

void EyeRenderer::render(DmaFollower& dma,
                         SharedRenderState* render_state,
                         ScopedProfilerNode& prof) {
  m_debug.clear();

  // skip if disabled
  if (!m_enabled) {
    while (dma.current_tag_offset() != render_state->next_bucket) {
      dma.read_and_advance();
    }
    return;
  }

  // jump to bucket
  auto data0 = dma.read_and_advance();
  ASSERT(data0.vif1() == 0);
  ASSERT(data0.vif0() == 0);
  ASSERT(data0.size_bytes == 0);

  // see if bucket is empty or not
  if (dma.current_tag().kind == DmaTag::Kind::CALL) {
    // renderer didn't run, let's just get out of here.
    for (int i = 0; i < 4; i++) {
      dma.read_and_advance();
    }
    ASSERT(dma.current_tag_offset() == render_state->next_bucket);
    return;
  }

  handle_eye_dma2(dma, render_state, prof);

  while (dma.current_tag_offset() != render_state->next_bucket) {
    auto data = dma.read_and_advance();
    m_debug += fmt::format("dma: {}\n", data.size_bytes);
  }
}

void EyeRenderer::draw_debug_window() {
  ImGui::Text("Time: %.3f ms\n", m_average_time_ms);
  ImGui::Text("Debug:\n%s", m_debug.c_str());
}

//////////////////////
// DMA Decode
//////////////////////

EyeRenderer::ScissorInfo decode_scissor(const DmaTransfer& dma) {
  ASSERT(dma.vif0() == 0);
  ASSERT(dma.vifcode1().kind == VifCode::Kind::DIRECT);
  ASSERT(dma.size_bytes == 32);

  GifTag gifTag(dma.data);
  ASSERT(gifTag.nloop() == 1);
  ASSERT(gifTag.eop());
  ASSERT(!gifTag.pre());
  ASSERT(gifTag.flg() == GifTag::Format::PACKED);
  ASSERT(gifTag.nreg() == 1);

  u8 reg_addr;
  memcpy(&reg_addr, dma.data + 24, 1);
  ASSERT((GsRegisterAddress)reg_addr == GsRegisterAddress::SCISSOR_1);
  EyeRenderer::ScissorInfo result;
  u64 val;
  memcpy(&val, dma.data + 16, 8);
  GsScissor reg(val);
  result.x0 = reg.x0();
  result.x1 = reg.x1();
  result.y0 = reg.y0();
  result.y1 = reg.y1();
  return result;
}

EyeRenderer::SpriteInfo decode_sprite(const DmaTransfer& dma) {
  /*
   (new 'static 'dma-gif-packet
        :dma-vif (new 'static 'dma-packet
                      :dma (new 'static 'dma-tag :qwc #x6 :id (dma-tag-id cnt))
                      :vif1 (new 'static 'vif-tag :imm #x6 :cmd (vif-cmd direct) :msk #x1)
                      )
        :gif0 (new 'static 'gif-tag64
                   :nloop #x1
                   :eop #x1
                   :pre #x1
                   :prim (new 'static 'gs-prim :prim (gs-prim-type sprite) :tme #x1 :fst #x1)
                   :nreg #x5
                   )
        :gif1 (new 'static 'gif-tag-regs
                   :regs0 (gif-reg-id rgbaq)
                   :regs1 (gif-reg-id uv)
                   :regs2 (gif-reg-id xyz2)
                   :regs3 (gif-reg-id uv)
                   :regs4 (gif-reg-id xyz2)
                   )
        )
   */

  ASSERT(dma.vif0() == 0);
  ASSERT(dma.vifcode1().kind == VifCode::Kind::DIRECT);
  ASSERT(dma.size_bytes == 6 * 16);

  // note: not checking everything here.
  GifTag gifTag(dma.data);
  ASSERT(gifTag.nloop() == 1);
  ASSERT(gifTag.eop());
  ASSERT(gifTag.pre());
  ASSERT(gifTag.flg() == GifTag::Format::PACKED);
  ASSERT(gifTag.nreg() == 5);

  EyeRenderer::SpriteInfo result;

  // rgba
  ASSERT(dma.data[16] == 128);               // r
  ASSERT(dma.data[16 + 4] == 128);           // r
  ASSERT(dma.data[16 + 8] == 128);           // r
  memcpy(&result.a, dma.data + 16 + 12, 1);  // a

  // uv0
  memcpy(&result.uv0, &dma.data[32], 8);

  // xyz0
  memcpy(&result.xyz0[0], &dma.data[48], 12);
  result.xyz0[2] >>= 4;

  // uv1
  memcpy(&result.uv1[0], &dma.data[64], 8);

  // xyz1
  memcpy(&result.xyz1[0], &dma.data[80], 12);
  result.xyz1[2] >>= 4;

  return result;
}

EyeRenderer::EyeDraw read_eye_draw(DmaFollower& dma) {
  auto scissor = decode_scissor(dma.read_and_advance());
  auto sprite = decode_sprite(dma.read_and_advance());
  return {sprite, scissor};
}

std::vector<EyeRenderer::SingleEyeDraws> EyeRenderer::get_draws(DmaFollower& dma,
                                                                SharedRenderState* render_state) {
  std::vector<SingleEyeDraws> draws;
  // now, loop over eyes. end condition is a 8 qw transfer to restore gs.
  while (dma.current_tag().qwc != 8) {
    draws.emplace_back();
    draws.emplace_back();

    auto& l_draw = draws[draws.size() - 2];
    auto& r_draw = draws[draws.size() - 1];

    l_draw.lr = 0;
    r_draw.lr = 1;

    // eye background setup
    auto adgif0_dma = dma.read_and_advance();
    ASSERT(adgif0_dma.size_bytes == 96);  // 5 adgifs a+d's plus tag
    ASSERT(adgif0_dma.vif0() == 0);
    ASSERT(adgif0_dma.vifcode1().kind == VifCode::Kind::DIRECT);
    AdgifHelper adgif0(adgif0_dma.data + 16);
    auto tex0 = render_state->texture_pool->lookup_gpu_texture(adgif0.tex0().tbp0());

    u32 pair_idx = -1;
    // first draw. this is the background. It reads 0,0 of the texture uses that color everywhere.
    // we'll also figure out the eye index here.
    bool using_64 = false;
    {
      auto draw0 = read_eye_draw(dma);
      // ASSERT(draw0.sprite.uv0[0] == 0);
      // ASSERT(draw0.sprite.uv0[1] == 0);
      // printf("hashed name is 0x%x 0x%x\n", draw0.sprite.uv0[0], draw0.sprite.uv0[1]);
      l_draw.fnv_name_hash = draw0.sprite.uv0;
      r_draw.fnv_name_hash = draw0.sprite.uv0;
      ASSERT(draw0.sprite.uv1[0] == 0);
      ASSERT(draw0.sprite.uv1[1] == 0);
      if (draw0.scissor.y1 - draw0.scissor.y0 == 63) {
        using_64 = true;
        l_draw.using_64 = true;
        r_draw.using_64 = true;
      }
      u32 y0 = (draw0.sprite.xyz0[1] - 512) >> 4;
      if (using_64) {
        y0 = (draw0.sprite.xyz0[1] - 1024) >> 5;
        y0 *= 4;
      }
      pair_idx = y0 / SINGLE_EYE_SIZE;
      l_draw.pair = pair_idx;
      r_draw.pair = pair_idx;
    }

    // up next is the pupil background
    {
      l_draw.iris = read_eye_draw(dma);
      l_draw.iris_tex = tex0;
      // FIX 106 (AI-assisted): TexturePool::lookup returns an empty optional when the
      // VRAM slot has no live texture (level evicted, mid-stream). The old code
      // blindly dereferenced it. value_or(0) yields GL name 0 = "nothing bound",
      // which run_gpu already tolerates (the *_tex null checks).
      l_draw.iris_gl_tex = render_state->texture_pool->lookup(adgif0.tex0().tbp0()).value_or(0);

      if (dma.current_tag().qwc == 6) {
        // change adgif!
        auto r_iris_adgif = dma.read_and_advance();
        ASSERT(r_iris_adgif.size_bytes == 96);  // 5 adgifs a+d's plus tag
        ASSERT(r_iris_adgif.vif0() == 0);
        ASSERT(r_iris_adgif.vifcode1().kind == VifCode::Kind::DIRECT);
        AdgifHelper r_iris_helper(r_iris_adgif.data + 16);
        r_draw.iris = read_eye_draw(dma);
        r_draw.iris_tex =
            render_state->texture_pool->lookup_gpu_texture(r_iris_helper.tex0().tbp0());
        r_draw.iris_gl_tex =
            render_state->texture_pool->lookup(r_iris_helper.tex0().tbp0()).value_or(0);
      } else {
        // same adgif
        r_draw.iris = read_eye_draw(dma);
        r_draw.iris_tex = tex0;
        r_draw.iris_gl_tex = l_draw.iris_gl_tex;
      }
    }

    // now we'll draw the pupil on top of that
    auto test1 = dma.read_and_advance();
    (void)test1;
    auto adgif1_dma = dma.read_and_advance();
    ASSERT(adgif1_dma.size_bytes == 96);  // 5 adgifs a+d's plus tag
    ASSERT(adgif1_dma.vif0() == 0);
    ASSERT(adgif1_dma.vifcode1().kind == VifCode::Kind::DIRECT);
    AdgifHelper adgif1(adgif1_dma.data + 16);
    auto tex1 = render_state->texture_pool->lookup_gpu_texture(adgif1.tex0().tbp0());

    // FIX 74 (AI-assisted): the pupil DMA transfers must ALWAYS be consumed.
    // The GOAL eye code always sends them; skipping read_eye_draw desyncs the DMA
    // walker and trips the scissor assert on the next eye draw. BCn textures
    // have no CPU-side pixel data (get_data_ptr() is null), but the renderer
    // only needs the GL texture handle, which is valid for compressed textures.
    l_draw.pupil = read_eye_draw(dma);
    if (tex1) {
      l_draw.pupil_tex = tex1;
      l_draw.pupil_gl_tex = render_state->texture_pool->lookup(adgif1.tex0().tbp0()).value_or(0);
    }

    if (dma.current_tag().qwc == 6) {
      auto r_pupil_adgif = dma.read_and_advance();
      ASSERT(r_pupil_adgif.size_bytes == 96);  // 5 adgifs a+d's plus tag
      ASSERT(r_pupil_adgif.vif0() == 0);
      ASSERT(r_pupil_adgif.vifcode1().kind == VifCode::Kind::DIRECT);
      AdgifHelper r_pupil_helper(r_pupil_adgif.data + 16);
      r_draw.pupil = read_eye_draw(dma);
      r_draw.pupil_tex =
          render_state->texture_pool->lookup_gpu_texture(r_pupil_helper.tex0().tbp0());
      r_draw.pupil_gl_tex =
          render_state->texture_pool->lookup(r_pupil_helper.tex0().tbp0()).value_or(0);
    } else {
      // FIX 74 (AI-assisted): always consume the right pupil DMA (see above).
      r_draw.pupil = read_eye_draw(dma);
      if (tex1) {
        r_draw.pupil_tex = tex1;
        r_draw.pupil_gl_tex = l_draw.pupil_gl_tex;
      }
    }

    // and finally the eyelid
    auto test2 = dma.read_and_advance();
    (void)test2;
    auto adgif2_dma = dma.read_and_advance();
    ASSERT(adgif2_dma.size_bytes == 96);  // 5 adgifs a+d's plus tag
    ASSERT(adgif2_dma.vif0() == 0);
    ASSERT(adgif2_dma.vifcode1().kind == VifCode::Kind::DIRECT);
    AdgifHelper adgif2(adgif2_dma.data + 16);
    auto tex2 = render_state->texture_pool->lookup_gpu_texture(adgif2.tex0().tbp0());

    {
      l_draw.lid = read_eye_draw(dma);
      l_draw.lid_tex = tex2;
      l_draw.lid_gl_tex = render_state->texture_pool->lookup(adgif2.tex0().tbp0()).value_or(0);
    }

    if (dma.current_tag().qwc == 6) {
      auto r_lid_adgif = dma.read_and_advance();
      ASSERT(r_lid_adgif.size_bytes == 96);  // 5 adgifs a+d's plus tag
      ASSERT(r_lid_adgif.vif0() == 0);
      ASSERT(r_lid_adgif.vifcode1().kind == VifCode::Kind::DIRECT);
      AdgifHelper r_lid_helper(r_lid_adgif.data + 16);
      r_draw.lid = read_eye_draw(dma);
      r_draw.lid_tex = render_state->texture_pool->lookup_gpu_texture(r_lid_helper.tex0().tbp0());
      r_draw.lid_gl_tex =
          render_state->texture_pool->lookup(r_lid_helper.tex0().tbp0()).value_or(0);
    } else {
      r_draw.lid = read_eye_draw(dma);
      r_draw.lid_tex = tex2;
      r_draw.lid_gl_tex = l_draw.lid_gl_tex;
    }

    if (render_state->version == GameVersion::Jak1) {
      auto end = dma.read_and_advance();
      ASSERT(end.size_bytes == 0);
      ASSERT(end.vif0() == 0);
      ASSERT(end.vif1() == 0);
    }
  }
  return draws;
}

void EyeRenderer::handle_eye_dma2(DmaFollower& dma,
                                  SharedRenderState* render_state,
                                  ScopedProfilerNode&) {
  Timer timer;
  m_debug.clear();

  // first should be the gs setup for render to texture
  auto offset_setup = dma.read_and_advance();
  ASSERT(offset_setup.size_bytes == 128);
  ASSERT(offset_setup.vifcode0().kind == VifCode::Kind::FLUSHA);
  ASSERT(offset_setup.vifcode1().kind == VifCode::Kind::DIRECT);

  // next should be alpha setup
  auto alpha_setup = dma.read_and_advance();
  ASSERT(alpha_setup.size_bytes == 32);
  ASSERT(alpha_setup.vifcode0().kind == VifCode::Kind::NOP);
  ASSERT(alpha_setup.vifcode1().kind == VifCode::Kind::DIRECT);

  if (render_state->version == GameVersion::Jak1) {
    // from the add to bucket
    ASSERT(dma.current_tag().kind == DmaTag::Kind::NEXT);
    ASSERT(dma.current_tag().qwc == 0);
    ASSERT(dma.current_tag_vif0() == 0);
    ASSERT(dma.current_tag_vif1() == 0);
    dma.read_and_advance();
  }

  auto draws = get_draws(dma, render_state);
  run_gpu(draws, render_state);

  float time_ms = timer.getMs();
  m_average_time_ms = m_average_time_ms * 0.95 + time_ms * 0.05;
}

int add_draw_to_buffer_32(int idx,
                          const EyeRenderer::EyeDraw& draw,
                          float* data,
                          int pair,
                          int lr) {
  int x_off = lr * SINGLE_EYE_SIZE * 16;
  int y_off = pair * SINGLE_EYE_SIZE * 16;

  data[idx++] = draw.sprite.xyz0[0] - x_off;
  data[idx++] = draw.sprite.xyz0[1] - y_off;
  data[idx++] = 0;
  data[idx++] = 0;

  data[idx++] = draw.sprite.xyz1[0] - x_off;
  data[idx++] = draw.sprite.xyz0[1] - y_off;
  data[idx++] = 1;
  data[idx++] = 0;

  data[idx++] = draw.sprite.xyz0[0] - x_off;
  data[idx++] = draw.sprite.xyz1[1] - y_off;
  data[idx++] = 0;
  data[idx++] = 1;

  data[idx++] = draw.sprite.xyz1[0] - x_off;
  data[idx++] = draw.sprite.xyz1[1] - y_off;
  data[idx++] = 1;
  data[idx++] = 1;
  return idx;
}

int add_draw_to_buffer_64(int idx,
                          const EyeRenderer::EyeDraw& draw,
                          float* data,
                          int pair,
                          int lr) {
  int x_off = lr * SINGLE_EYE_SIZE * 32;
  int y_off = (pair / 4) * SINGLE_EYE_SIZE * 32;

  data[idx++] = (draw.sprite.xyz0[0] - x_off) / 2;
  data[idx++] = (draw.sprite.xyz0[1] - y_off) / 2;
  data[idx++] = 0;
  data[idx++] = 0;

  data[idx++] = (draw.sprite.xyz1[0] - x_off) / 2;
  data[idx++] = (draw.sprite.xyz0[1] - y_off) / 2;
  data[idx++] = 1;
  data[idx++] = 0;

  data[idx++] = (draw.sprite.xyz0[0] - x_off) / 2;
  data[idx++] = (draw.sprite.xyz1[1] - y_off) / 2;
  data[idx++] = 0;
  data[idx++] = 1;

  data[idx++] = (draw.sprite.xyz1[0] - x_off) / 2;
  data[idx++] = (draw.sprite.xyz1[1] - y_off) / 2;
  data[idx++] = 1;
  data[idx++] = 1;
  return idx;
}

int add_clear_draw_to_buffer(int idx, float* data) {
  // the entire eye texture is cleared using the 0,0 value from the iris texture
  const float center = 768;
  const float upper = center + 256;
  const float lower = center - 256;
  data[idx++] = lower;
  data[idx++] = lower;
  data[idx++] = 0;
  data[idx++] = 0;

  data[idx++] = upper;
  data[idx++] = lower;
  data[idx++] = 0;
  data[idx++] = 0;

  data[idx++] = lower;
  data[idx++] = upper;
  data[idx++] = 0;
  data[idx++] = 0;

  data[idx++] = upper;
  data[idx++] = upper;
  data[idx++] = 0;
  data[idx++] = 0;
  return idx;
}

void EyeRenderer::run_gpu(const std::vector<SingleEyeDraws>& draws,
                          SharedRenderState* render_state) {
  if (draws.empty()) {
    return;
  }
  // FIX 106 (AI-assisted): the first draw's slot indexes m_gpu_eye_textures below
  // (FramebufferTexturePairContext + the loop). Verify it before touching the array.
  if (safe_eye_slot(draws.front().tex_slot()) < 0) {
    return;
  }

  glBindVertexArray(m_vao);
  glBindBuffer(GL_ARRAY_BUFFER, m_gl_vertex_buffer);

  // the first thing we'll do is prepare the vertices
  int buffer_idx = 0;
  for (const auto& draw : draws) {
    buffer_idx = add_clear_draw_to_buffer(buffer_idx, m_gpu_vertex_buffer);
    if (draw.using_64) {
      buffer_idx =
          add_draw_to_buffer_64(buffer_idx, draw.iris, m_gpu_vertex_buffer, draw.pair, draw.lr);
      buffer_idx =
          add_draw_to_buffer_64(buffer_idx, draw.pupil, m_gpu_vertex_buffer, draw.pair, draw.lr);
      buffer_idx =
          add_draw_to_buffer_64(buffer_idx, draw.lid, m_gpu_vertex_buffer, draw.pair, draw.lr);
    } else {
      buffer_idx =
          add_draw_to_buffer_32(buffer_idx, draw.iris, m_gpu_vertex_buffer, draw.pair, draw.lr);
      buffer_idx =
          add_draw_to_buffer_32(buffer_idx, draw.pupil, m_gpu_vertex_buffer, draw.pair, draw.lr);
      buffer_idx =
          add_draw_to_buffer_32(buffer_idx, draw.lid, m_gpu_vertex_buffer, draw.pair, draw.lr);
    }
  }
  ASSERT(buffer_idx <= VTX_BUFFER_FLOATS);
  int check = buffer_idx;

  // maybe buffer sub data.
  glBufferData(GL_ARRAY_BUFFER, buffer_idx * sizeof(float), m_gpu_vertex_buffer, GL_STREAM_DRAW);

  FramebufferTexturePairContext ctxt(m_gpu_eye_textures[draws.front().tex_slot()].fb);

  // set up common opengl state
  glDisable(GL_DEPTH_TEST);
  render_state->shaders[ShaderId::EYE].activate();
  glUniform1i(gl_uniform_loc(render_state->shaders[ShaderId::EYE].id(), "tex_T0"), 0);
  glActiveTexture(GL_TEXTURE0);

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  // FIX 43 (AI-assisted): sampler state overrides texture state, so a background
  // sampler must not still be bound when we configure the texture by hand.
  background_sampler_unbind();
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

  buffer_idx = 0;
  for (size_t draw_idx = 0; draw_idx < draws.size(); draw_idx++) {
    const auto& draw = draws[draw_idx];
    // FIX 106 (AI-assisted): bounds-check the screen-Y-derived pair index before it
    // indexes m_gpu_eye_textures (out-of-bounds write otherwise).
    const int slot_idx = safe_eye_slot(draw.tex_slot());
    if (slot_idx < 0) {
      continue;
    }
    auto& out_tex = m_gpu_eye_textures[slot_idx];
    out_tex.fnv_name_hash = draw.fnv_name_hash;
    out_tex.lr = draw.lr;
    // FIX 106 (AI-assisted): keep the stable GL name and the recent-hash cache fresh
    // so Merc2 lookups for this character succeed even after the slot is reused.
    if (!out_tex.gl_tex) {
      out_tex.gl_tex = out_tex.fb.texture();
    }
    remember_eye_hash(out_tex.fnv_name_hash, out_tex.lr, out_tex.gl_tex);

    // clear: not really needed, but we do it to help debugging in case all the textures are missing
    float clear[4] = {1.0, 0, 0, 0};
    glClearBufferfv(GL_COLOR, 0, clear);

    // background
    if (draw.iris_tex) {
      glDisable(GL_BLEND);
      glBindTexture(GL_TEXTURE_2D, draw.iris_gl_tex);
      gfx::count_draw(4);
      glDrawArrays(GL_TRIANGLE_STRIP, buffer_idx / 4, 4);
    }
    buffer_idx += 4 * 4;

    // iris
    if (draw.iris_tex) {
      // set alpha
      // set Z
      // set texture
      glDisable(GL_BLEND);
      glBindTexture(GL_TEXTURE_2D, draw.iris_gl_tex);
      gfx::count_draw(4);
      glDrawArrays(GL_TRIANGLE_STRIP, buffer_idx / 4, 4);
    }
    buffer_idx += 4 * 4;

    if (draw.pupil_tex) {
      glEnable(GL_BLEND);
      glBlendEquation(GL_FUNC_ADD);
      glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      glBindTexture(GL_TEXTURE_2D, draw.pupil_gl_tex);
      gfx::count_draw(4);
      glDrawArrays(GL_TRIANGLE_STRIP, buffer_idx / 4, 4);
    }
    buffer_idx += 4 * 4;

    if (draw.lid_tex) {
      glDisable(GL_BLEND);
      glBindTexture(GL_TEXTURE_2D, draw.lid_gl_tex);
      gfx::count_draw(4);
      glDrawArrays(GL_TRIANGLE_STRIP, buffer_idx / 4, 4);
    }
    buffer_idx += 4 * 4;

    // finally, give to "vram"
    render_state->texture_pool->move_existing_to_vram(out_tex.gpu_tex, out_tex.tbp);

    if (draw_idx != draws.size() - 1) {
      // FIX 106 (AI-assisted): bounds-check the next draw's slot too.
      const int next_slot = safe_eye_slot(draws[draw_idx + 1].tex_slot());
      if (next_slot >= 0) {
        ctxt.switch_to(m_gpu_eye_textures[next_slot].fb);
      }
    }
  }

  ASSERT(check == buffer_idx);

  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
}

std::optional<u64> EyeRenderer::lookup_eye_texture(u8 eye_id) {
  eye_id = (eye_id % 40);
  if ((s32)eye_id >= NUM_EYE_PAIRS * 2) {
    log_eye_lookup_failed("(1)", eye_id);
    return {};
  }
  // FIX 106 (AI-assisted): return our own stable GL name for the slot. The old code
  // dereferenced the TexturePool's GpuTexture* (gpu_tex->gpu_textures.at(0).gl),
  // which dangles if the pool ever invalidates that wrapper -- a READ of freed heap
  // on the render thread.
  const GpuEyeTex& slot = m_gpu_eye_textures[eye_id];
  if (slot.gpu_tex && slot.gl_tex) {
    return slot.gl_tex;
  }
  log_eye_lookup_failed("", eye_id);
  return {};
}

void EyeRenderer::remember_eye_hash(u64 hash, bool lr, u64 gl_tex) {
  if (!hash || !gl_tex) {
    return;
  }
  HashCacheEntry& e = m_hash_cache[m_hash_cache_next];
  e.hash = hash;
  e.lr = lr;
  e.gl_tex = gl_tex;
  m_hash_cache_next = (m_hash_cache_next + 1) % HASH_CACHE_SIZE;
}

std::optional<u64> EyeRenderer::lookup_eye_texture_hash(u64 hash, bool lr) {
  // Live slots first: the freshest composite of exactly this character's eyes.
  for (auto& slot : m_gpu_eye_textures) {
    if (slot.fnv_name_hash == hash && slot.lr == lr) {
      if (slot.gl_tex) {
        return slot.gl_tex;
      }
      log_eye_lookup_failed("(1)", hash);
      return {};
    }
  }
  // FIX 106 (AI-assisted): then the recent-resolution cache. With more than
  // NUM_EYE_PAIRS eye-visible characters the slot was overwritten by another
  // character's eyes (jak3 stadium/desert crowds) -- the old code failed here on
  // every frame and flooded the SD card with "lookup eye failed" lines.
  for (int i = 0; i < HASH_CACHE_SIZE; i++) {
    const HashCacheEntry& e = m_hash_cache[i];
    if (e.hash == hash && e.lr == lr && e.gl_tex) {
      return e.gl_tex;
    }
  }
  log_eye_lookup_failed("(2)", hash);
  return {};
}

//////////////////////
// DMA Decode
//////////////////////

std::string EyeRenderer::SpriteInfo::print() const {
  std::string result;
  result += fmt::format("a: {:x} uv: ({}), ({}, {}) xyz: ({}, {}, {}), ({}, {}, {})", a, uv0,
                        uv1[0], uv1[1], xyz0[0], xyz0[1], xyz0[2], xyz1[0], xyz1[1], xyz1[2]);
  return result;
}

std::string EyeRenderer::ScissorInfo::print() const {
  return fmt::format("x : [{}, {}], y : [{}, {}]", x0, x1, y0, y1);
}

std::string EyeRenderer::EyeDraw::print() const {
  return fmt::format("{}\n{}\n", sprite.print(), scissor.print());
}