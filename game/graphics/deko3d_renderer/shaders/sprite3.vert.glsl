#version 310 es
precision highp float;
precision highp int;

// T79 (AI-assisted): deko3d port of opengl_renderer/shaders/sprite3_3d.vert (loose uniforms ->
// std140, jak1 SCISSOR_ADJUST * HEIGHT_SCALE = (512.0/448.0) * 1.0).
layout(std140, binding = 7) uniform SpriteVert {
  vec4 hvdf_offset;
  mat4 camera;
  mat4 hud_matrix;
  vec4 hud_hvdf_offset;
  vec4 hud_hvdf_user[75];
  vec4 basis_x;
  vec4 basis_y;
  vec4 xy_array[8];
  vec4 xyz_array[4];
  vec4 st_array[4];
  vec4 consts0;  // pfog0, fog_min, fog_max, min_scale
  vec4 consts1;  // max_scale, deg_to_rad, inv_area, 0
};

layout (location = 0) in vec4 xyz_sx;
layout (location = 1) in vec4 quat_sy;
layout (location = 2) in vec4 rgba;
layout (location = 3) in uvec2 flags_matrix;
layout (location = 4) in uvec4 tex_info_in;

flat out vec4 fragment_color;
out vec3 tex_coord;
flat out uvec2 tex_info;

vec4 matrix_transform(mat4 mtx, vec3 pt) {
  return mtx[3] + mtx[0] * pt.x + mtx[1] * pt.y + mtx[2] * pt.z;
}

mat3 sprite_quat_to_rot(vec3 quat) {
  mat3 result;
  float qr = sqrt(abs(1.0 - (quat.x * quat.x + quat.y * quat.y + quat.z * quat.z)));
  result[0][0] = 1.0 - 2.0 * (quat.y * quat.y + quat.z * quat.z);
  result[1][0] = 2.0 * (quat.x * quat.y - quat.z * qr);
  result[2][0] = 2.0 * (quat.x * quat.z + quat.y * qr);
  result[0][1] = 2.0 * (quat.x * quat.y + quat.z * qr);
  result[1][1] = 1.0 - 2.0 * (quat.x * quat.x + quat.z * quat.z);
  result[2][1] = 2.0 * (quat.y * quat.z - quat.x * qr);
  result[0][2] = 2.0 * (quat.x * quat.z - quat.y * qr);
  result[1][2] = 2.0 * (quat.y * quat.z + quat.x * qr);
  result[2][2] = 1.0 - 2.0 * (quat.x * quat.x + quat.y * quat.y);
  return result;
}

vec4 sprite_transform2(vec3 root, vec4 off, mat3 sprite_rot, float sx, float sy) {
  vec3 pos = root;
  vec3 offset = sprite_rot[0] * off.x * sx + sprite_rot[1] * off.y + sprite_rot[2] * off.z * sy;
  pos += offset;
  vec4 transformed_pos = -matrix_transform(camera, pos);
  float Q = consts0.x / transformed_pos.w;
  transformed_pos.xyz *= Q;
  transformed_pos.xyz += hvdf_offset.xyz;
  return transformed_pos;
}

void main() {
  float pfog0 = consts0.x;
  float fog_min = consts0.y;
  float min_scale = consts0.w;
  float max_scale = consts1.x;
  float deg_to_rad = consts1.y;
  float inv_area = consts1.z;

  vec3 position = xyz_sx.xyz;
  float sx = xyz_sx.w;
  float sy = quat_sy.w;
  fragment_color = rgba;
  uint vert_id = tex_info_in.z;
  uint rendermode = tex_info_in.w;  // 1 = 2D, 2 = HUD, 3 = 3D
  vec3 quat = quat_sy.xyz;
  uint matrix = flags_matrix.y;

  vec4 transformed = vec4(0.0);
  vec4 transformed_pos_vf02 = matrix_transform(rendermode == 2u ? hud_matrix : camera, position);
  float Q = pfog0 / transformed_pos_vf02.w;

  vec4 scales_vf01 = xyz_sx;
  scales_vf01.z = sy;
  scales_vf01.zw *= Q;
  scales_vf01.x = scales_vf01.z;
  scales_vf01.x *= scales_vf01.w;
  scales_vf01.x *= inv_area;
  fragment_color.w *= min(scales_vf01.x, 1.0);

  if (rendermode == 3u) {
    mat3 rot = sprite_quat_to_rot(quat);
    transformed = sprite_transform2(position, xyz_array[vert_id], rot, sx, sy);
  } else if (rendermode == 1u) {
    transformed_pos_vf02.xyz *= Q;
    vec4 offset_pos_vf10 = transformed_pos_vf02 + hvdf_offset;
    offset_pos_vf10.w = max(offset_pos_vf10.w, fog_min);
    scales_vf01.z = clamp(scales_vf01.z, min_scale, max_scale);
    scales_vf01.w = clamp(scales_vf01.w, min_scale, max_scale);
    quat.z *= deg_to_rad;
    float sp_sin = sin(quat.z);
    float sp_cos = cos(quat.z);
    vec4 xy0_vf19 = xy_array[vert_id + (flags_matrix.x & 15u)];
    vec4 vf12_rotated = (basis_x * sp_cos) - (basis_y * sp_sin);
    vec4 vf13_rotated_trans = (basis_x * sp_sin) + (basis_y * sp_cos);
    vf12_rotated *= scales_vf01.w;
    vf13_rotated_trans *= scales_vf01.z;
    transformed = offset_pos_vf10 + vf12_rotated * xy0_vf19.x + vf13_rotated_trans * xy0_vf19.y;
  } else if (rendermode == 2u) {
    transformed_pos_vf02.xyz *= Q;
    vec4 offset_pos_vf10 = transformed_pos_vf02 +
        (matrix == 0u ? hud_hvdf_offset : hud_hvdf_user[min(matrix - 1u, 74u)]);
    scales_vf01.z = max(scales_vf01.z, min_scale);
    scales_vf01.w = max(scales_vf01.w, min_scale);
    quat.z *= deg_to_rad;
    float sp_sin = sin(quat.z);
    float sp_cos = cos(quat.z);
    vec4 xy0_vf19 = xy_array[vert_id + (flags_matrix.x & 15u)];
    vec4 vf12_rotated = (basis_x * sp_cos) - (basis_y * sp_sin);
    vec4 vf13_rotated_trans = (basis_x * sp_sin) + (basis_y * sp_cos);
    vf12_rotated *= scales_vf01.w;
    vf13_rotated_trans *= scales_vf01.z;
    transformed = offset_pos_vf10 + vf12_rotated * xy0_vf19.x + vf13_rotated_trans * xy0_vf19.y;
  }

  tex_coord = st_array[vert_id].xyz;

  transformed.xy -= (2048.);
  transformed.z /= (8388608.);
  transformed.z -= 1.;
  transformed.x /= (256.);
  transformed.y /= -(128.);
  transformed.xyz *= transformed.w;
  transformed.y *= (512.0/448.0) * 1.0;
  gl_Position = transformed;

  fragment_color *= 2.;
  fragment_color.w *= 2.;
  tex_info = tex_info_in.xy;
}
