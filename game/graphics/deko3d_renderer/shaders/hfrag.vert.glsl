#version 310 es
// T113 (AI-assisted): dk port of opengl_renderer/shaders/hfrag.vert (jak1 Hfrag,
// the distant mountain terrain -- GL background/Hfrag.cpp, bucket 8).
// Differences from the GL original (and the only ones):
//  - #version 310 es + precision (dk convention)
//  - the loose camera uniforms move into the std140 VertParams block shared
//    with tfrag3 (dk_tfrag_ubo.h, binding 7); unused members keep the layout
//  - position arrives as ONE float (the height) like the GL shader reads it;
//    x/z come from the 512x512 grid index vi
//  - SCISSOR_ADJUST / HEIGHT_SCALE tokens are substituted by gen-dksh.py
//    (jak1: 1.0 and 512.0/448.0)
precision highp float;
precision highp int;

layout(std140, binding = 7) uniform HfragVert {
  vec4 hvdf_offset;    // @0   (w used)
  vec4 cam_trans;      // @16
  mat4 pc_camera;      // @32
  mat4 camera;         // @96  (unused by hfrag, keeps the std140 layout)
  float fog_constant;  // @160 (unused)
  float fog_min;       // @164
  float fog_max;       // @168
  int decal;           // @172 (unused)
};

layout (location = 0) in float height_in;
layout (location = 1) in uint color_index_in;  // u16 tod palette index
layout (location = 2) in uvec2 uv_in;          // 2 x u8, each 0 or 1
layout (location = 3) in uint vi_in;           // grid index: x = vi % 512, z = vi / 512

layout (binding = 0) uniform sampler2D tex_T10;  // time-of-day palette (vertex stage)

out vec4 fragment_color;
out vec2 tex_coord;
out float fogginess;

void main() {
  int vx = int(vi_in) % 512;
  int vz = int(vi_in) / 512;

  tex_coord.x = (uv_in.x == 1u) ? 1.f : 0.f;
  tex_coord.y = (uv_in.y == 1u) ? 1.f : 0.f;

  vec4 transformed = -pc_camera[3];
  transformed -= pc_camera[0] * (32768.f * float(vx) - cam_trans.x);
  transformed -= pc_camera[1] * (height_in - cam_trans.y);
  transformed -= pc_camera[2] * (32768.f * float(vz) - cam_trans.z);

  fogginess = 255. - clamp(-transformed.w + hvdf_offset.w, fog_min, fog_max);

  // scissoring area adjust
  transformed.y *= SCISSOR_ADJUST * HEIGHT_SCALE;
  gl_Position = transformed;

  // time of day lookup
  fragment_color = texelFetch(tex_T10, ivec2(int(color_index_in), 0), 0);
  fragment_color.a = 1.0;
}
