#version 310 es
// T113 (AI-assisted): dk port of opengl_renderer/shaders/hfrag.frag.
// Differences from the GL original (and the only ones):
//  - #version 310 es + precision (dk convention)
//  - fog_color / gfx_hack_no_tex move into the std140 FragParams block shared
//    with tfrag3 (dk_tfrag_ubo.h, binding 7); alpha_min/alpha_max stay in the
//    block for layout parity but the hfrag path has no alpha test (GL has none)
precision highp float;
precision highp int;

layout(std140, binding = 7) uniform HfragFrag {
  float alpha_min;       // @0  (unused)
  float alpha_max;       // @4  (unused)
  vec4 fog_color;        // @16 rgb + intensity
  int gfx_hack_no_tex;   // @32
};

layout(binding = 0) uniform sampler2D tex_T0;  // the bucket's montage texture

in vec4 fragment_color;
in vec2 tex_coord;
in float fogginess;

out vec4 color;

void main() {
  if (gfx_hack_no_tex == 0) {
    vec4 T0 = texture(tex_T0, tex_coord);
    color = fragment_color * T0 * 2.;
  } else {
    color = fragment_color;
  }
  color.rgb = mix(color.rgb, fog_color.rgb, clamp(fogginess * fog_color.a, 0., 1.));
}
