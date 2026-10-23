#version 310 es
// T113 (AI-assisted): dk port of opengl_renderer/shaders/hfrag_montage.frag.
// Only difference: #version 310 es + precision (dk convention).
precision highp float;

layout(binding = 0) uniform sampler2D tex_T0;

in vec2 uv;
out vec4 color;

void main() {
  vec4 t = texture(tex_T0, uv);
  color = vec4(t.rgb, 1.);
}
