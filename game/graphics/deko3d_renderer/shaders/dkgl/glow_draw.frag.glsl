#version 310 es
precision highp float;
precision highp int;
layout(std140, binding = 7) uniform FragParams {
  float glow_boost;
  float _pad_glow_boost_0;
  float _pad_glow_boost_1;
  float _pad_glow_boost_2;
};

out vec4 color;

layout(location = 0) in vec4 fragment_color;
layout(location = 2) in float discard_flag;
layout(location = 1) in vec2 uv_texture;

layout(binding = 0) uniform highp sampler2D tex_T0;



void main() {
  vec4 texture_color = texture(tex_T0, uv_texture);
  color.xyz = texture_color.xyz * fragment_color.xyz * 2.f * discard_flag / 128.f * glow_boost;
  color.w = fragment_color.w * texture_color.w;
}
