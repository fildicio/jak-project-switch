#version 310 es
precision highp float;
precision highp int;
layout(std140, binding = 7) uniform FragParams {
  vec4 fog_color;
  int ignore_alpha;
  float _pad_ignore_alpha_0;
  float _pad_ignore_alpha_1;
  float _pad_ignore_alpha_2;
  int decal_enable;
  float _pad_decal_enable_0;
  float _pad_decal_enable_1;
  float _pad_decal_enable_2;
  int gfx_hack_no_tex;
  float _pad_gfx_hack_no_tex_0;
  float _pad_gfx_hack_no_tex_1;
  float _pad_gfx_hack_no_tex_2;
};

out vec4 color;
layout(location = 0) in vec3 vtx_color;
layout(location = 1) in vec2 vtx_st;
layout(location = 2) in float fog;


layout(binding = 0) uniform highp sampler2D tex_T0;









void main() {
  if (gfx_hack_no_tex == 0) {
    vec4 T0 = texture(tex_T0, vtx_st);

    color.a = T0.a;
    color.rgb = T0.rgb * vtx_color;
    color *= 2.;
  } else {
    color.rgb = vtx_color;
    color.a = 1.;
  }
}
