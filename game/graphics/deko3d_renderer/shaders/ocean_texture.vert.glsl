#version 310 es
// T95 (AI-assisted): dk port of opengl_renderer/shaders/ocean_texture.vert.
// Differences from the GL original (and the only ones):
//  - #version 310 es + precision (dk convention)
//  - the static position stream (loc 0) and the dynamic s/t/rgba stream
//    (loc 1 rgba unorm u8x4 @8, loc 2 st float2 @0) live in TWO vertex
//    buffers (GL used two GL_ARRAY_BUFFER binds as well)
precision highp float;
precision highp int;

layout (location = 0) in vec2 position_in;
layout (location = 1) in vec4 rgba_in;
layout (location = 2) in vec2 tex_coord_in;

out vec4 fragment_color;
out vec2 tex_coord;

void main() {
  // inputs are 0 - 2048.
  gl_Position = vec4((position_in.x - 1024.) / 1024., (position_in.y - 1024.) / 1024., 0.5, 1.0);
  fragment_color = vec4(rgba_in.rgb * 2., 1.);
  tex_coord = tex_coord_in;
}
