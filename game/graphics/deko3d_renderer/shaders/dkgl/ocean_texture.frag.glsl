#version 310 es
precision highp float;
precision highp int;

out vec4 color;
in vec2 tex_coord;
in vec4 fragment_color;

layout(binding = 0) uniform highp sampler2D tex_T0;

void main() {
  vec4 tex = texture(tex_T0, tex_coord);
  color = fragment_color * tex;
}
