#version 310 es
precision highp float;

// T82 (AI-assisted): deko3d port of opengl_renderer/shaders/eye.frag.
layout(binding = 0) uniform sampler2D tex_T0;

in vec2 st;
out vec4 color;

void main() {
  color = texture(tex_T0, st);
  color.w *= 2.0;
}
