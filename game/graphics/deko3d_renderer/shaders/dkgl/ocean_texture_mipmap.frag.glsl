#version 310 es
precision highp float;
precision highp int;
layout(std140, binding = 7) uniform FragParams {
  float alpha_intensity;
  float _pad_alpha_intensity_0;
  float _pad_alpha_intensity_1;
  float _pad_alpha_intensity_2;
};

out vec4 color;
layout(location = 0) in vec2 tex_coord;


layout(binding = 0) uniform highp sampler2D tex_T0;

void main() {
  vec4 tex = texture(tex_T0, tex_coord);
  tex.w *= alpha_intensity;
  color = tex;
}
