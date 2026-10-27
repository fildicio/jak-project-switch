#version 310 es
precision highp float;
precision highp int;
layout(early_fragment_tests) in;

// #17 (AI-assisted): sprite3.frag with early depth/stencil tests. Used only for sprite draws that
// do not write depth: then running the depth test before the shader (and its discard) gives the
// same picture, but hidden particle pixels are never shaded or blended (Geyser smash overdraw).
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
