#version 310 es
// T113 (AI-assisted): dk port of opengl_renderer/shaders/hfrag_montage.vert.
// Only difference: #version 310 es + precision (dk convention).
precision highp float;

layout (location = 0) in vec2 pos;
layout (location = 1) in vec2 st;

out vec2 uv;

void main() {
  gl_Position = vec4(pos.x * 2. - 1., pos.y * 2. - 1., 0., 1.);
  uv = st;
}
