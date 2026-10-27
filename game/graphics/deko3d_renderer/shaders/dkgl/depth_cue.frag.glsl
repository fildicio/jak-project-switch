#version 310 es
precision highp float;
precision highp int;

layout(location = 0) flat in vec4 fragment_color;
layout(location = 1) in vec2 tex_coord;

layout(binding = 0) uniform highp sampler2D tex;

out vec4 out_color;

void main() {
  // sample texture
  out_color = fragment_color * texture(tex, tex_coord);
}
