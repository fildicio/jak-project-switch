#pragma once

#include "common/math/Vector.h"

#include "game/graphics/opengl_renderer/BucketRenderer.h"

struct GoalBackgroundCameraData {
  math::Vector4f planes[4];
  math::Vector<s32, 4> itimes[4];
  math::Vector4f camera[4];
  math::Vector4f hvdf_off;
  math::Vector4f fog;
  math::Vector4f trans;
  math::Vector4f rot[4];
  math::Vector4f perspective[4];
};

// data passed from game to PC renderers
// the GOAL code assumes this memory layout.
struct TfragPcPortData {
  GoalBackgroundCameraData camera;
  char level_name[32];
};
static_assert(sizeof(TfragPcPortData) == 16 * 25);

// inputs to background renderers.
struct TfragRenderSettings {
  GoalBackgroundCameraData camera;
  int tree_idx;
  bool debug_culling = false;
  const u8* occlusion_culling = nullptr;
};

enum class DoubleDrawKind { NONE, AFAIL_NO_DEPTH_WRITE };

struct DoubleDraw {
  DoubleDrawKind kind = DoubleDrawKind::NONE;
  float aref_first = 0.;
  float aref_second = 0.;
  float color_mult = 1.;
};

/*!
 * Uniform locations used by the tfrag-style background shaders. Fixed after link, so they
 * are looked up once per ShaderId and cached (see get_tfrag_shader_uniforms).
 */
struct TfragShaderUniforms {
  bool initialized = false;
  GLuint program = 0;
  GLint gfx_hack_no_tex = -1;
  GLint decal = -1;
  GLint tex_T0 = -1;
  GLint camera = -1;
  GLint pc_camera = -1;
  GLint hvdf_offset = -1;
  GLint cam_trans = -1;
  GLint fog_constant = -1;
  GLint fog_min = -1;
  GLint fog_max = -1;
  GLint fog_color = -1;
  GLint alpha_min = -1;
  GLint alpha_max = -1;
};

/*!
 * FIX 29 (Switch perf): the tfrag/tie/shrub/hfrag draw loops used to call
 * glGetUniformLocation on every draw (setup_tfrag_shader) and every tree/pass setup
 * (first_tfrag_draw_setup) -- several hundred driver round-trips per frame on the render
 * thread. Locations never change after the shader is linked, so they are looked up once
 * per ShaderId, lazily on first use. Shaders are created before the first frame, and this
 * is only called from the render thread, so the lazy fill is race-free. Uniforms the
 * shader doesn't have stay -1; glUniform* with -1 is a no-op per the GL spec, exactly
 * like the previous if (u_id != -1) guards.
 */
const TfragShaderUniforms& get_tfrag_shader_uniforms(SharedRenderState* render_state,
                                                     ShaderId shader);

/*!
 * A2a (Switch perf): setup_opengl_from_draw_mode used to re-issue ~10 fixed-function GL
 * calls (active texture, depth test/func, blend equation+funcs+color, depth mask and
 * 4x glTexParameteri) on every background/merc/sprite draw. It now mirrors the state it
 * applied and skips redundant calls:
 *  - the global part (depth test/func, blend, depth mask) is keyed by the DrawMode bits
 *    that affect it, and is skipped when the key is unchanged;
 *  - the sampler part (glTexParameteri) is keyed by the clamp/filter bits + mipmap and
 *    is additionally skipped only when the caller reports that the texture bound to
 *    tex_unit did not change since the previous call (texture_rebound == false).
 * The mirror is only valid between calls within a single renderer pass: every pass that
 * uses this function must start with reset_draw_mode_state_cache(), because other
 * renderers freely modify the same global GL state between buckets. Callers that do not
 * track texture rebinding pass texture_rebound = true (default) and always get a full
 * sampler apply, exactly like the old behavior.
 */
DoubleDraw setup_tfrag_shader(SharedRenderState* render_state,
                              DrawMode mode,
                              ShaderId shader,
                              bool texture_rebound = true);
DoubleDraw setup_opengl_from_draw_mode(DrawMode mode,
                                       u32 tex_unit,
                                       bool mipmap,
                                       bool texture_rebound = true);

/*!
 * A2a (Switch perf): forget the mirrored draw-mode state. Call at the start of every
 * render pass that uses setup_opengl_from_draw_mode / setup_tfrag_shader (see the
 * comment above for why).
 */
void reset_draw_mode_state_cache();

void first_tfrag_draw_setup(const GoalBackgroundCameraData& settings,
                            SharedRenderState* render_state,
                            ShaderId shader);

void interp_time_of_day(const math::Vector<s32, 4> itimes[4],
                        const tfrag3::PackedTimeOfDay& packed_colors,
                        math::Vector<u8, 4>* out);

void cull_check_all_slow(const math::Vector4f* planes,
                         const std::vector<tfrag3::VisNode>& nodes,
                         const u8* level_occlusion_string,
                         u8* out);
bool sphere_in_view_ref(const math::Vector4f& sphere, const math::Vector4f* planes);

void update_render_state_from_pc_settings(SharedRenderState* state, const TfragPcPortData& data);

void make_all_visible_multidraws(std::pair<int, int>* draw_ptrs_out,
                                 GLsizei* counts_out,
                                 void** index_offsets_out,
                                 const std::vector<tfrag3::ShrubDraw>& draws);

u32 make_all_visible_multidraws(std::pair<int, int>* draw_ptrs_out,
                                GLsizei* counts_out,
                                void** index_offsets_out,
                                const std::vector<tfrag3::StripDraw>& draws);

u32 make_multidraws_from_vis_string(std::pair<int, int>* draw_ptrs_out,
                                    GLsizei* counts_out,
                                    void** index_offsets_out,
                                    const std::vector<tfrag3::StripDraw>& draws,
                                    const std::vector<u8>& vis_data);

u32 make_all_visible_index_list(std::pair<int, int>* group_out,
                                u32* idx_out,
                                const std::vector<tfrag3::StripDraw>& draws,
                                const u32* idx_in,
                                u32* num_tris_out);

u32 make_index_list_from_vis_string(std::pair<int, int>* group_out,
                                    u32* idx_out,
                                    const std::vector<tfrag3::StripDraw>& draws,
                                    const std::vector<u8>& vis_data,
                                    const u32* idx_in,
                                    u32* num_tris_out);

u32 make_all_visible_index_list(std::pair<int, int>* group_out,
                                u32* idx_out,
                                const std::vector<tfrag3::ShrubDraw>& draws,
                                const u32* idx_in);

u32 make_multidraws_from_vis_and_proto_string(std::pair<int, int>* draw_ptrs_out,
                                              GLsizei* counts_out,
                                              void** index_offsets_out,
                                              const std::vector<tfrag3::StripDraw>& draws,
                                              const std::vector<u8>& vis_data,
                                              const std::vector<u8>& proto_vis_data);

u32 make_index_list_from_vis_and_proto_string(std::pair<int, int>* group_out,
                                              u32* idx_out,
                                              const std::vector<tfrag3::StripDraw>& draws,
                                              const std::vector<u8>& vis_data,
                                              const std::vector<u8>& proto_vis_data,
                                              const u32* idx_in,
                                              u32* num_tris_out);
