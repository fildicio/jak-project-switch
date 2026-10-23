#version 310 es
// T95 (AI-assisted): dk port of opengl_renderer/shaders/ocean_texture.frag.
// Only difference from the GL original: #version 310 es + precision, and the
// explicit sampler binding 0 (dk convention; GL used glActiveTexture 0).
precision highp float;
precision highp int;

layout(binding = 0) uniform sampler2D tex_T0;

in vec2 tex_coord;
in vec4 fragment_color;

out vec4 color;

void main() {
  vec4 tex = texture(tex_T0, tex_coord);
  color = fragment_color * tex;
}
