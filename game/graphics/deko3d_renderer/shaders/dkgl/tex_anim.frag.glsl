#version 310 es
precision highp float;
precision highp int;
layout(std140, binding = 7) uniform FragParams {
  vec4 rgba;
  int enable_tex;
  float _pad_enable_tex_0;
  float _pad_enable_tex_1;
  float _pad_enable_tex_2;
  int set_alpha;
  float _pad_set_alpha_0;
  float _pad_set_alpha_1;
  float _pad_set_alpha_2;
  int tcc;
  float _pad_tcc_0;
  float _pad_tcc_1;
  float _pad_tcc_2;
  ivec4 channel_scramble;
  float alpha_multiply;
  float _pad_alpha_multiply_0;
  float _pad_alpha_multiply_1;
  float _pad_alpha_multiply_2;
  float minimum;
  float _pad_minimum_0;
  float _pad_minimum_1;
  float _pad_minimum_2;
  float maximum;
  float _pad_maximum_0;
  float _pad_maximum_1;
  float _pad_maximum_2;
  float slime_scroll;
  float _pad_slime_scroll_0;
  float _pad_slime_scroll_1;
  float _pad_slime_scroll_2;
};

out vec4 color;











layout(location = 0) in vec2 uv;

layout(binding = 0) uniform highp sampler2D tex_T0;
// GLES has no 1D textures/samplers at all -- the C++ side now creates this as a GL_TEXTURE_2D
// with height 1, so indexing needs an explicit y=0 via ivec2 below.
layout(binding = 10) uniform highp sampler2D tex_T10;

float cloud_lookup(float v, float minimum, float maximum) {
  maximum = max(minimum, maximum);
  if (v <= minimum) {
    return 0.0;
  }
  if (v >= maximum) {
    return 1.0;
  }

  float alpha = (v - minimum) / (maximum - minimum);
  float sin_alpha = sin(alpha * 3.1415926 / 2.f);
  return sin_alpha * sin_alpha;
}

void main() {
  if (enable_tex == 1) {
    vec4 tex_color = texture(tex_T0, uv);
    vec4 unscambled_tex = vec4(tex_color[channel_scramble[0]],
    tex_color[channel_scramble[1]],
    tex_color[channel_scramble[2]],
    tex_color[channel_scramble[3]]);
    color = rgba / 128.;
    if (tcc == 1) {
      color *= unscambled_tex;
    } else {
      color.xyz *= unscambled_tex.xyz;
    }
  } else if (enable_tex == 2) {
    // cloud version
    vec4 tex_color = texture(tex_T0, uv);
    color.x = 0.5;
    color.y = 0.5;
    color.z = 0.5;
    color.a = 0.5 * cloud_lookup(tex_color.r, minimum, maximum);
  } else if (enable_tex == 3) {
    // cloud version
    vec4 tex_color = texture(tex_T0, uv + vec2(0, slime_scroll));
    color = texelFetch(tex_T10, ivec2(int(tex_color.r * 255.f), 0), 0);
  } else {
    color = (rgba / 128.);
  }
  if (set_alpha == 1) {
    color.a = 0.5;
  } else {
    color.a *= alpha_multiply;
  }
}