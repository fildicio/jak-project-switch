#version 310 es
precision highp float;
precision highp int;

// T79 (AI-assisted): deko3d port of opengl_renderer/shaders/sprite3_3d.frag (loose uniforms ->
// std140).
layout(std140, binding = 7) uniform SpriteFrag {
  vec4 alpha_range;  // x = alpha_min, y = alpha_max
};

out vec4 color;

flat in vec4 fragment_color;
in vec3 tex_coord;
flat in uvec2 tex_info;

layout(binding = 0) uniform sampler2D tex_T0;

void main() {
  vec4 T0 = texture(tex_T0, tex_coord.xy);
  if (tex_info.y == 0u) {
    T0.w = 1.0;
  }
  color = fragment_color * T0;
  if (color.a < alpha_range.x || color.a > alpha_range.y) {
    discard;
  }
}
