#version 310 es
precision highp float;
precision highp int;
layout(std140, binding = 7) uniform FragParams {
  float alpha_reject;
  float _pad_alpha_reject_0;
  float _pad_alpha_reject_1;
  float _pad_alpha_reject_2;
  float color_mult;
  float _pad_color_mult_0;
  float _pad_color_mult_1;
  float _pad_color_mult_2;
  vec4 fog_color;
  int gfx_hack_no_tex;
  float _pad_gfx_hack_no_tex_0;
  float _pad_gfx_hack_no_tex_1;
  float _pad_gfx_hack_no_tex_2;
  uint warp_sample_mode;
  float _pad_warp_sample_mode_0;
  float _pad_warp_sample_mode_1;
  float _pad_warp_sample_mode_2;
};


out vec4 color;
layout(location = 0) in vec2 tex_coord;




layout(location = 2) in float fog;
layout(location = 1) in vec4 fragment_color;

layout(location = 3) flat in uvec2 tex_info;




layout(binding = 0) uniform highp sampler2D tex_T0;

vec4 sample_tex(vec2 coord, uint unit) {
  return texture(tex_T0, coord);
}

void main() {
  // 0x1 is tcc
  // 0x2 is decal
  // 0x4 is fog

  if (warp_sample_mode == 1u || gfx_hack_no_tex == 0) {
    vec4 T0 = sample_tex(tex_coord.xy, tex_info.x);
    if ((tex_info.y & 1u) == 0u) {
      if ((tex_info.y & 2u) == 0u) {
        // modulate + no tcc
        color.rgb = fragment_color.rgb * T0.rgb;
        color.a = fragment_color.a;
      } else {
        // decal + no tcc
        color.rgb = T0.rgb * 0.5;
        color.a = fragment_color.a;
      }
    } else {
      if ((tex_info.y & 2u) == 0u) {
        // modulate + tcc
        color = fragment_color * T0;
      } else {
        // decal + tcc
        color.rgb = T0.rgb * 0.5;
        color.a = T0.a;
      }
    }
    color *= 2.;
  } else {
    if ((tex_info.y & 1u) == 0u) {
      if ((tex_info.y & 2u) == 0u) {
        // modulate + no tcc
        color.rgb = fragment_color.rgb;
        color.a = fragment_color.a * 2.;
      } else {
        // decal + no tcc
        color.rgb = vec3(1);
        color.a = fragment_color.a * 2.;
      }
    } else {
      if ((tex_info.y & 2u) == 0u) {
        // modulate + tcc
        color = fragment_color;
      } else {
        // decal + tcc
        color.rgb = vec3(0.5);
        color.a = 1.0;
      }
    }
  }
  color.rgb *= color_mult;

  if (color.a < alpha_reject) {
    discard;
  }
  if ((tex_info.y & 4u) != 0u) {
    color.xyz = mix(color.xyz, fog_color.rgb, clamp(fog_color.a * fog, 0., 1.));
  }
}
