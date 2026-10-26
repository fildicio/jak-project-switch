#version 310 es
precision highp float;
precision highp int;
layout(std140, binding = 7) uniform VertParams {
  float scale;
};

layout (location = 0) in vec2 position_in;
layout (location = 1) in vec2 tex_coord_in;

out vec2 tex_coord;

void main() {
  gl_Position = vec4(position_in.x * scale - (1. - scale), position_in.y * scale - (1. - scale), 0.5, 1.0);
  tex_coord = tex_coord_in;
}
