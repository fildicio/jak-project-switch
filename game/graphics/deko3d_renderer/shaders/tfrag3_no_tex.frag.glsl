#version 310 es
precision highp float;
precision highp int;

layout(std140, binding = 7) uniform FragParams {
  float alpha_min;
  float alpha_max;
  vec4 fog_color;
  int gfx_hack_no_tex;
};


out vec4 color;

in vec4 fragment_color;
in vec3 tex_coord;
layout(binding = 0) uniform sampler2D tex_T0;


void main() {
  color = fragment_color;
}
