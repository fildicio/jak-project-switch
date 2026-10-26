#version 310 es
precision highp float;
precision highp int;
layout(std140, binding = 7) uniform FragParams {
  float color_mult;
  float alpha_mult;
  vec4 fog_color;
  int bucket;
};

out vec4 color;

in vec4 fragment_color;
in vec3 tex_coord;






in float fog;

layout(binding = 0) uniform highp sampler2D tex_T0;

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
