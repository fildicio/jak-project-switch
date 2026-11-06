#version 310 es
precision highp float;
precision highp int;

layout (location = 0) in vec4 position_in;
layout (location = 1) in vec4 rgba_in;
layout (location = 2) in vec2 uv_texture_in;
layout (location = 3) in vec2 uv_probe_in;

layout(location = 0) out vec4 fragment_color;
layout(location = 1) out vec2 uv_texture;
layout(location = 2) out float discard_flag;

layout(binding = 1) uniform highp sampler2D tex_T1;


void main() {
  vec4 transformed = position_in;
  transformed.xy -= (2048.);
  transformed.z /= (8388608.);
  transformed.z -= 1.;
  transformed.x /= (256.);
  transformed.y /= -(128.);
  transformed.xyz *= transformed.w;
  // scissoring area adjust
  transformed.y *= (512.0 / 448.0) * 1.0;
  gl_Position = transformed;
  fragment_color = rgba_in;
  uv_texture = uv_texture_in;
  discard_flag = texture(tex_T1, uv_probe_in).a;
}
