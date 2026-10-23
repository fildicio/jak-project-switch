#version 310 es
// FIX 112 (AI-assisted): dk port of opengl_renderer/shaders/ocean_texture_mipmap.vert.
// Differences from the GL original (and the only ones):
//  - #version 310 es + precision (dk convention)
//  - GL shrinks the quad with the `scale` uniform into a fixed 128x128 viewport
//    anchored bottom-left; the dk port instead draws a fullscreen quad into a
//    per-level viewport sized to the level itself - same texel coverage, so the
//    scale uniform is gone.
precision highp float;

layout (location = 0) in vec2 position_in;
layout (location = 1) in vec2 tex_coord_in;

out vec2 tex_coord;

void main() {
  gl_Position = vec4(position_in, 0.5, 1.0);
  tex_coord = tex_coord_in;
}
