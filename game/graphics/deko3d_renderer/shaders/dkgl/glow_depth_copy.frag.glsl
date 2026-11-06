#version 310 es
precision highp float;
precision highp int;

out vec4 out_color;

layout(binding = 0) uniform highp sampler2D tex;

layout(location = 0) in vec2 tex_coord;

void main() {
  vec2 texture_coords = vec2(tex_coord.x, tex_coord.y);
  out_color = vec4(0., 0., 0., 0.);
  if (texture_coords.x < 0. || texture_coords.x > 1. || texture_coords.y > 1. || texture_coords.y < 0.) {
    gl_FragDepth = 1.;
  } else {
    gl_FragDepth = texture(tex, texture_coords).r;
  }
}
