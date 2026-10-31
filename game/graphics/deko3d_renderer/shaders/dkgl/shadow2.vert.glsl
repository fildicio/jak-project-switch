#version 310 es
precision highp float;
precision highp int;
layout(std140, binding = 7) uniform VertParams {
  int clear_mode;
  float _pad_clear_mode_0;
  float _pad_clear_mode_1;
  float _pad_clear_mode_2;
  float fog;
  float _pad_fog_0;
  float _pad_fog_1;
  float _pad_fog_2;
  vec4 perspective_x;
  vec4 perspective_y;
  vec4 perspective_z;
  vec4 perspective_w;
  vec4 hvdf_offset;
};

layout (location = 0) in vec3 position_in;










void main() {
  if (clear_mode == 1) {
    // this is just copied from shadow 1, needs revisiting.
    gl_Position = vec4((position_in.x - 0.5) * 16., -(position_in.y - 0.5) * 32., position_in.z * 2. - 1., 1.0);
    gl_Position.y *= (512.0 / 448.0);
  } else {
    vec4 transformed = -perspective_w;
    transformed -= perspective_x * position_in.x;
    transformed -= perspective_y * position_in.y;
    transformed -= perspective_z * position_in.z;
    transformed.xyz *= fog / transformed.w;


    transformed.xyz += hvdf_offset.xyz;
    transformed.xy -= (2048.);
    transformed.z /= (8388608.);
    transformed.z -= 1.;
    transformed.x /= (256.);
    transformed.y /= -(128.);
    transformed.xyz *= transformed.w;
    transformed.y *= (512.0 / 448.0) * 1.0;
    gl_Position = transformed;
  }
}
