#version 310 es
precision highp float;
precision highp int;

// T78 (AI-assisted): deko3d port of opengl_renderer/shaders/merc2.vert (loose uniforms -> std140).
layout(std140, binding = 7) uniform MercVert {
  vec4 light_dir0_fade;
  vec4 light_dir1_fade_en;
  vec4 light_dir2;
  vec4 light_col0;
  vec4 light_col1;
  vec4 light_col2;
  vec4 light_ambient;
  vec4 hvdf_offset;
  vec4 fog_constants;
  mat4 perspective_matrix;
};

struct MercMatrixData {
  mat4 X;
  mat3 R;
  vec4 pad;
};

layout(std140, binding = 8) uniform MercBones {
  MercMatrixData bones[128];
};

layout (location = 0) in vec3 position_in;
layout (location = 1) in vec3 normal_in;
layout (location = 2) in vec3 weights_in;
layout (location = 3) in vec2 st_in;
layout (location = 4) in vec4 rgba;
layout (location = 5) in uvec3 mats;

out vec4 vtx_color;
out vec2 vtx_st;
out float fog;

void main() {
  vec4 p = vec4(position_in, 1.);
  vec4 vtx_pos = -bones[mats[0]].X * p * weights_in[0];
  vec3 rotated_nrm = bones[mats[0]].R * normal_in * weights_in[0];

  // game may send garbage bones if the weight is 0, don't let NaNs sneak in.
  if (weights_in[1] > 0.) {
    vtx_pos += -bones[mats[1]].X * p * weights_in[1];
    rotated_nrm += bones[mats[1]].R * normal_in * weights_in[1];
  }
  if (weights_in[2] > 0.) {
    vtx_pos += -bones[mats[2]].X * p * weights_in[2];
    rotated_nrm += bones[mats[2]].R * normal_in * weights_in[2];
  }

  vec4 transformed = perspective_matrix * vtx_pos;

  rotated_nrm = normalize(rotated_nrm);
  vec3 light_intensity = light_dir0_fade.xyz * rotated_nrm.x + light_dir1_fade_en.xyz * rotated_nrm.y + light_dir2.xyz * rotated_nrm.z;
  light_intensity = max(light_intensity, vec3(0., 0., 0.));

  vec4 light_color = light_ambient
                   + light_intensity.x * light_col0
                   + light_intensity.y * light_col1
                   + light_intensity.z * light_col2;

  float Q = fog_constants.x / transformed[3];
  fog = 255. - clamp(-transformed.w + hvdf_offset.w, fog_constants.y, fog_constants.z);

  transformed.xyz *= Q;
  transformed.xyz += hvdf_offset.xyz;
  transformed.xy -= (2048.);
  transformed.z /= (8388608.);
  transformed.z -= 1.;
  transformed.x /= (256.);
  transformed.y /= -(128.);
  transformed.xyz *= transformed.w;
  transformed.y *= (512.0/448.0) * 1.0;
  gl_Position = transformed;

  vtx_color = rgba * light_color;
  vtx_st = st_in;
}
