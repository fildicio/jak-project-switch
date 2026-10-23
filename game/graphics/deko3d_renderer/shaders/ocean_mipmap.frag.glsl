#version 310 es
// FIX 112 (AI-assisted): dk port of opengl_renderer/shaders/ocean_texture_mipmap.frag.
// Differences from the GL original (and the only ones):
//  - #version 310 es + precision (dk convention)
//  - the loose `alpha_intensity` uniform moves into the std140 OceanMip block
//    (binding 7). GL fades alpha by max(0, 1 - 0.51*i) per level so the envmap
//    reflection (the DST_ALPHA blends) fades out with distance.
precision highp float;

layout(std140, binding = 7) uniform OceanMip {
  vec4 params;  // x = alpha_intensity
};

layout(binding = 0) uniform sampler2D tex_T0;

in vec2 tex_coord;
out vec4 color;

void main() {
  vec4 tex = texture(tex_T0, tex_coord);
  color = vec4(tex.rgb, tex.a * params.x);
}
