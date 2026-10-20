#version 310 es
precision highp float;

// T82 (AI-assisted): deko3d port of opengl_renderer/shaders/eye.vert (render-to-texture eyes).
layout(location = 0) in vec4 xyst_in;

out vec2 st;

void main() {
  gl_Position = vec4((xyst_in.x - 768.0) / 256.0, (xyst_in.y - 768.0) / 256.0, 0.0, 1.0);
  st = xyst_in.zw;
}
