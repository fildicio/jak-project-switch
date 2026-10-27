#version 310 es
precision highp float;
precision highp int;

out vec4 color;
layout(location = 0) in vec2 st;
layout(binding = 0) uniform highp sampler2D tex_T0;

void main() {
  color = texture(tex_T0, st);
  color.w *= 2.;
}
