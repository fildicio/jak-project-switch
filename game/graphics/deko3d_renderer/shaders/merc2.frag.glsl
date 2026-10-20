#version 310 es
precision highp float;
precision highp int;

// T78 (AI-assisted): deko3d port of opengl_renderer/shaders/merc2.frag (loose uniforms -> std140).
layout(std140, binding = 7) uniform MercFrag {
  vec4 fog_color;
  int ignore_alpha;
  int decal_enable;
  int gfx_hack_no_tex;
  float fade;      // light_dir0_fade.w
  float fade_en;   // light_dir1_fade_en.w
};

out vec4 color;
in vec4 vtx_color;
in vec2 vtx_st;
in float fog;

layout(binding = 0) uniform sampler2D tex_T0;

void main() {
  if (gfx_hack_no_tex == 0) {
    vec4 T0 = texture(tex_T0, vtx_st);
    // all merc is tcc=rgba and modulate
    if (decal_enable == 0) {
      color = vtx_color * T0 * 2.;
    } else {
      color = T0;
    }
    color.a *= 2.;
  } else {
    color.rgb = vtx_color.rgb;
    if (decal_enable == 0) {
      color.a = vtx_color.a * 2.;
    } else {
      color.a = 1.;
    }
  }

  if (fade_en > 0.) {
    color.a = fade;
  } else if (fade_en < 0.) {
    color.a *= fade;
  }

  if (ignore_alpha == 0 && color.w < 0.128) {
    discard;
  }

  color.xyz = mix(color.xyz, fog_color.rgb, clamp(fog_color.a * fog, 0., 1.));
}
