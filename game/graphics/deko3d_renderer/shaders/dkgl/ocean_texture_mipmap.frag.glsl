#version 310 es
precision highp float;
precision highp int;
layout(std140, binding = 7) uniform FragParams {
  float alpha_intensity;
};

out vec4 color;
in vec2 tex_coord;


layout(binding = 0) uniform highp sampler2D tex_T0;

void main() {
  vec4 tex = texture(tex_T0, tex_coord);
  tex.w *= alpha_intensity;
  color = tex;
}
