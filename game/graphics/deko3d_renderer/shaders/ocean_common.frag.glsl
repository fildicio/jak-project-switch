#version 310 es
// T95 (AI-assisted): dk port of opengl_renderer/shaders/ocean_common.frag.
// Differences from the GL original (and the only ones):
//  - #version 310 es + precision (dk convention)
//  - fog_color / bucket move into the std140 OceanCommon block (binding 7,
//    vertex stage uses the same data at binding 6)
//  - the dead color_mult / alpha_mult uniforms are dropped (the GL shader
//    never reads them)
precision highp float;
precision highp int;

layout(std140, binding = 7) uniform OceanCommonFrag {
  vec4 fog_color;
  int bucket;
};

layout(binding = 0) uniform sampler2D tex_T0;

in vec4 fragment_color;
in vec3 tex_coord;
in float fog;

out vec4 color;

void main() {
  vec4 T0 = texture(tex_T0, tex_coord.xy / tex_coord.z);
  if (bucket == 0) {
    color.rgb = fragment_color.rgb * T0.rgb;
    color.a = fragment_color.a;
    color.rgb = mix(color.rgb, fog_color.rgb, clamp(fog_color.a * fog, 0., 1.));
  } else if (bucket == 1 || bucket == 2 || bucket == 4) {
    color = fragment_color * T0;
  } else if (bucket == 3) {
    color = fragment_color * T0;
    color.rgb = mix(color.rgb, fog_color.rgb, clamp(fog_color.a * fog, 0., 1.));
  }
}
