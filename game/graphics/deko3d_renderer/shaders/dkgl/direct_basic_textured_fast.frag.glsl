// HAND-WRITTEN variant of the generated direct_basic_textured.frag.glsl (T103) --
// NOT owned by scripts/ai/dkgl_gen_shaders.py (its --check only covers the files it
// generates for its PROGRAMS list). Keep in sync with that file's sampling/blending
// logic; any change there must be mirrored here.
//
// Switch perf (AI-assisted): identical color output, but with NO discard at all:
//  - the GS scissor block is gone -- dkgl only binds this shader when the draw's
//    scissor_enable uniform is 0;
//  - the alpha-test blocks are gone -- dkgl only binds this shader when the
//    alpha_min/alpha_max window rejects nothing (the folded jak2 sky double-draw
//    uses [-10, 10], and draws with no alpha test use [0, 10]).
// A fragment shader without discard keeps early-Z rejection enabled on the Switch
// GPU; the double alpha-test discard is what made jak2 sky bucket 5 cost 6-9 ms.
// The uniform block and vertex inputs are kept IDENTICAL to the base shader so
// the same UBO slice and vertex layout drive both variants (unused members/inputs
// are deliberate).
#version 310 es
precision highp float;
precision highp int;
layout(std140, binding = 7) uniform FragParams {
  float alpha_min;
  float _pad_alpha_min_0;
  float _pad_alpha_min_1;
  float _pad_alpha_min_2;
  float alpha_max;
  float _pad_alpha_max_0;
  float _pad_alpha_max_1;
  float _pad_alpha_max_2;
  float color_mult;
  float _pad_color_mult_0;
  float _pad_color_mult_1;
  float _pad_color_mult_2;
  float alpha_mult;
  float _pad_alpha_mult_0;
  float _pad_alpha_mult_1;
  float _pad_alpha_mult_2;
  float alpha_sub;
  float _pad_alpha_sub_0;
  float _pad_alpha_sub_1;
  float _pad_alpha_sub_2;
  float ta0;
  float _pad_ta0_0;
  float _pad_ta0_1;
  float _pad_ta0_2;
  int scissor_enable;
  float _pad_scissor_enable_0;
  float _pad_scissor_enable_1;
  float _pad_scissor_enable_2;
  bool greater;
  float _pad_greater_0;
  float _pad_greater_1;
  float _pad_greater_2;
  vec4 game_sizes;
  vec4 fog_color;
};

out vec4 color;

layout(location = 0) in vec4 fragment_color;
layout(location = 1) in vec3 tex_coord;
layout(location = 5) flat in uint use_uv;
layout(location = 3) in vec4 gs_scissor;  // unused: only bound when scissor is off

// game width, game height, viewport width, viewport height

layout(location = 4) flat in uvec4 tex_info;
layout(location = 2) in float fog;

layout(binding = 20) uniform highp sampler2D tex_T20;

vec4 sample_tex(vec2 coord, uint unit) {
  return texture(tex_T20, coord);
}

vec4 sample_tex_px(vec2 coordf, uint unit) {
  // note: there is still fractional texels and filtering in this mode.
  vec2 coord_px = coordf / 16.f;
  vec2 tex_size = vec2(textureSize(tex_T20, 0));
  // but texture perspective correction is disabled.
  // current uses are on quads with the same z so it doesn't really matter.
  return textureProj(tex_T20, vec4(coord_px / tex_size, 1., 1.));
}

void main() {
  vec4 T0;
  if (use_uv == 1u) {
    T0 = sample_tex_px(tex_coord.xy, tex_info.x);
  } else {
    T0 = sample_tex(tex_coord.xy / tex_coord.z, tex_info.x);
  }
  // y is tcc
  // z is decal
  if (T0.w == 0.) {
    T0.w = ta0;
  }

  if (tex_info.y == 0u) {
    if (tex_info.z == 0u) {
      // modulate + no tcc
      color.xyz = fragment_color.xyz * T0.xyz;
      color.w = fragment_color.w;
    } else {
      // decal + no tcc
      color.xyz = T0.xyz * 0.5;
      color.w = fragment_color.w;
    }
  } else {
    if (tex_info.z == 0u) {
      // modulate + tcc
      color = fragment_color * T0;
    } else {
      // decal + tcc
      color.xyz = T0.xyz * 0.5;
      color.w = T0.w;
    }
  }
  color *= 2.;
  color.xyz *= color_mult;
  color.w *= alpha_mult;
  if (tex_info.w == 1u) {
    color.xyz = mix(color.xyz, fog_color.rgb, clamp(fog_color.a * fog, 0., 1.));
  }

}