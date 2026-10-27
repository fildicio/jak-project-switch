#version 310 es
precision highp float;
precision highp int;
layout(std140, binding = 7) uniform VertParams {
  float scale;
  float _pad_scale_0;
  float _pad_scale_1;
  float _pad_scale_2;
};

layout (location = 0) in vec2 position_in;
layout (location = 1) in vec2 tex_coord_in;

layout(location = 0) out vec2 tex_coord;

void main() {
  gl_Position = vec4(position_in.x * scale - (1. - scale), position_in.y * scale - (1. - scale), 0.5, 1.0);
  tex_coord = tex_coord_in;
}
