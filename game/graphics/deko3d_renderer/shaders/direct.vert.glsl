#version 310 es
precision highp float;
precision highp int;

// T80 (AI-assisted): deko3d port of opengl_renderer/shaders/direct_basic_textured.vert
// (jak1: HEIGHT_SCALE = 1.0, SCISSOR_ADJUST = 512/448, no offscreen mode).
layout(location = 0) in vec4 position_in;
layout(location = 1) in vec4 rgba_in;
layout(location = 2) in vec3 tex_coord_in;
layout(location = 3) in uvec4 tex_info_in;
layout(location = 4) in uint use_uv_in;
layout(location = 5) in vec4 gs_scissor_in;

out vec4 fragment_color;
out vec3 tex_coord;
out float fog;
out vec4 gs_scissor;
flat out uvec4 tex_info;
flat out uint use_uv;

void main() {
  gl_Position = vec4((position_in.x - 0.5) * 16., -(position_in.y - 0.5) * 32.,
                     position_in.z * 2. - 1., 1.0);
  gl_Position.y *= 512.0 / 448.0;
  fragment_color = vec4(rgba_in.x, rgba_in.y, rgba_in.z, rgba_in.w * 2.);
  tex_coord = tex_coord_in;
  tex_info = tex_info_in;
  fog = 255. - position_in.w;
  use_uv = use_uv_in;
  gs_scissor = gs_scissor_in;
}
