#version 310 es
// T95 (AI-assisted): dk port of opengl_renderer/shaders/ocean_common.vert.
// Differences from the GL original (and the only ones):
//  - #version 310 es + precision (dk convention)
//  - the loose `uniform int bucket` moves into the std140 OceanCommon block
//    (binding 6 here, binding 7 in the fragment shader; one 32-byte buffer)
//  - SCISSOR_ADJUST / HEIGHT_SCALE tokens are substituted by gen-dksh.py
//    (jak1: 1.0 and 512.0/448.0), same as sprite3/direct.
precision highp float;
precision highp int;

layout(std140, binding = 6) uniform OceanCommonVert {
  vec4 fog_color;
  int bucket;
};

layout (location = 0) in vec3 position_in;
layout (location = 1) in vec4 rgba_in;
layout (location = 2) in vec3 tex_coord_in;
layout (location = 3) in uint fog_in;

out vec4 fragment_color;
out vec3 tex_coord;
out float fog;

void main() {
  gl_Position = vec4((position_in.x - 0.5) * 16., -(position_in.y - 0.5) * 32., position_in.z * 2. - 1., 1.0);
  // scissoring area adjust
  gl_Position.y *= SCISSOR_ADJUST * HEIGHT_SCALE;
  fragment_color = vec4(rgba_in.rgb, rgba_in.a * 2.);
  tex_coord = tex_coord_in;
  fog = 255. - float(fog_in);

  if (bucket == 0) {
    fragment_color.rgb *= 2.;
  } else if (bucket == 1 || bucket == 3) {
    fragment_color *= 2.;
  } else if (bucket == 4) {
    fragment_color.a = 0.;
  }
}
