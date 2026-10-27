#version 310 es
precision highp float;
precision highp int;
layout(std140, binding = 7) uniform FragParams {
  int scissor_enable;
  float _pad_scissor_enable_0;
  float _pad_scissor_enable_1;
  float _pad_scissor_enable_2;
  vec4 game_sizes;
};

out vec4 color;

layout(location = 0) in vec4 fragment_color;
layout(location = 1) in vec4 gs_scissor;

// game width, game height, viewport width, viewport height


void main() {
  if (scissor_enable == 1) {
    float x = gl_FragCoord.x;
    float y = gl_FragCoord.y;
    float w = (game_sizes.z / game_sizes.x);
    float h = (game_sizes.w / game_sizes.y);
    float scax0 = gs_scissor.x * w + 0.5;
    float scax1 = gs_scissor.y * w + 0.5;
    float scay0 = (game_sizes.y - gs_scissor.w) * h + 0.5;
    float scay1 = (game_sizes.y - gs_scissor.z) * h + 0.5;
    if (x < scax0 || x > scax1) discard;
    if (y < scay0 || y > scay1) discard;
  }

  color = fragment_color;
}
