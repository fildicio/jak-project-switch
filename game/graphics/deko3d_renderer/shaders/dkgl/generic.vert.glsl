#version 310 es
precision highp float;
precision highp int;
layout(std140, binding = 7) uniform VertParams {
  float mat_32;
  float _pad_mat_32_0;
  float _pad_mat_32_1;
  float _pad_mat_32_2;
  vec3 fog_constants;
  float _pad_fog_constants_0;
  vec4 scale;
  float mat_23;
  float _pad_mat_23_0;
  float _pad_mat_23_1;
  float _pad_mat_23_2;
  float mat_33;
  float _pad_mat_33_0;
  float _pad_mat_33_1;
  float _pad_mat_33_2;
  bool use_full_matrix;
  float _pad_use_full_matrix_0;
  float _pad_use_full_matrix_1;
  float _pad_use_full_matrix_2;
  mat4 full_matrix;
  vec4 hvdf_offset;
  uint warp_sample_mode;
  float _pad_warp_sample_mode_0;
  float _pad_warp_sample_mode_1;
  float _pad_warp_sample_mode_2;
};

layout (location = 0) in vec3 position_in;
layout (location = 1) in vec4 rgba_in;
layout (location = 2) in vec2 tex_coord_in;
layout (location = 3) in uvec4 byte_info;











layout(location = 0) out vec2 tex_coord;

layout(location = 1) out vec4 fragment_color;
layout(location = 2) out float fog;

layout(location = 3) flat out uvec2 tex_info;

const float warp_off = 1.0f - (448.0 / 512.0f);

void main() {
  // lq.xy vf22, 0(vi10)          texture load?
  // lq_buffer(Mask::xy, vu.vf22, vu.vi10);

  // lq.xyz vf16, 2(vi10)         vertex load
  // lq_buffer(Mask::xyz, gen.vtx_load0, vu.vi10 + 2);

  // mtir vi02, vf22.x            grab s coordinate of texture
  // vu.vi02 = vu.vf22.x_as_u16();

  // mulaw.xyzw ACC, vf11, vf00   matrix multiply W
  // vu.acc.mula(Mask::xyzw, gen.mat3, vu.vf00.w());
  vec4 transformed;

  // maddax.xyzw ACC, vf08, vf16  matrix multiply X
  // vu.acc.madda(Mask::xyzw, gen.mat0, gen.vtx_load0.x());
  if (use_full_matrix) {
    transformed = -full_matrix[3];
    transformed -= full_matrix[0] * position_in.x ;
    transformed -= full_matrix[1] * position_in.y;
    transformed -= full_matrix[2] * position_in.z ;
  } else {
    transformed.xyz = position_in * scale.xyz;
    transformed.z += mat_32;
    transformed.w = mat_23 * position_in.z + mat_33;
    transformed *= -1.;
  }


  // div Q, vf01.x, vf12.w        perspective divide
  // vu.Q = gen.fog.x() / gen.vtx_p0.w();
  float Q = fog_constants.x / transformed.w;

  fog = 255. - clamp(-transformed.w + hvdf_offset.w, fog_constants.y, fog_constants.z);

  // itof12.xyz vf18, vf22        texture int to float
  // vu.vf18.itof12(Mask::xyz, vu.vf22);
  tex_coord = tex_coord_in / 4096.f;

  if (warp_sample_mode == 1u) {
    tex_coord = vec2(tex_coord.x, (1.0f - tex_coord.y - warp_off) * (512.0 / 448.0));
  }

  // mul.xyz vf12, vf12, Q        persepective divide
  // gen.vtx_p0.mul(Mask::xyz, gen.vtx_p0, vu.Q);
  transformed.xyz *= Q;

  // mul.xyz vf18, vf18, Q        texture perspective divide
  // vu.vf18.mul(Mask::xyz, vu.vf18, vu.Q);

  // add.xyzw vf12, vf12, vf04    apply hvdf
  // gen.vtx_p0.add(Mask::xyzw, gen.vtx_p0, gen.hvdf_off);
  transformed.xyz += hvdf_offset.xyz;

  // correct xy offset
  transformed.xy -= (2048.);

  // correct z scale
  transformed.z /= (8388608.);
  transformed.z -= 1.;

  // correct xy scale
  transformed.x /= (256.);
  transformed.y /= -(128.);

  // hack
  transformed.xyz *= transformed.w;

  gl_Position = transformed;
  // scissoring area adjust
  gl_Position.y *= (512.0 / 448.0) * 1.0;

  fragment_color = vec4(rgba_in.rgb, rgba_in.a * 2.);
  tex_info = byte_info.xy;
}
