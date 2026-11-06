#version 310 es
precision highp float;
precision highp int;

out vec4 out_color;

layout(binding = 0) uniform highp sampler2D tex;

layout(location = 0) in vec2 tex_coord;

void main() {
  vec2 texture_coords = vec2(tex_coord.x, tex_coord.y);

  // sample framebuffer texture
  out_color = texture(tex, texture_coords);
}
