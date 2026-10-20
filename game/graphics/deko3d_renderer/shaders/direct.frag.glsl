#version 310 es
precision highp float;
precision highp int;

// T80 (AI-assisted): deko3d port of opengl_renderer/shaders/direct_basic_textured.frag and
// direct_basic.frag (one shader, params.mult.z selects textured). Loose uniforms -> std140.
layout(std140, binding = 6) uniform DirectConsts {
  vec4 game_sizes;  // game w, game h, viewport w, viewport h
  vec4 fog_color;   // rgb, intensity
};
layout(std140, binding = 7) uniform DirectFrag {
  vec4 alpha;  // alpha_min, alpha_max, greater, ta0
  vec4 mult;   // color_mult, alpha_mult, textured, scissor_enable
  vec4 fix;    // x = constant output alpha, < 0 = off
};

out vec4 color;

in vec4 fragment_color;
in vec3 tex_coord;
in float fog;
in vec4 gs_scissor;
flat in uvec4 tex_info;
flat in uint use_uv;

layout(binding = 0) uniform sampler2D tex_T20;

void main() {
  if (mult.w > 0.5) {
    float x = gl_FragCoord.x;
    float y = gl_FragCoord.y;
    float w = (game_sizes.z / game_sizes.x);
    float h = (game_sizes.w / game_sizes.y);
    float scax0 = gs_scissor.x * w + 0.5;
    float scax1 = gs_scissor.y * w + 0.5;
    float scay0 = (game_sizes.y - gs_scissor.w) * h + 0.5;
    float scay1 = (game_sizes.y - gs_scissor.z) * h + 0.5;
    if (x < scax0 || x > scax1) discard;
    if (y < scay0 || y > scay1) discard;
  }

  if (mult.z < 0.5) {
    color = fragment_color;
    if (fix.x >= 0.) {
      color.w = fix.x;
    }
    return;
  }

  vec4 T0;
  if (use_uv == 1u) {
    vec2 tex_size = vec2(textureSize(tex_T20, 0));
    T0 = textureProj(tex_T20, vec4(tex_coord.xy / 16. / tex_size, 1., 1.));
  } else {
    T0 = texture(tex_T20, tex_coord.xy / tex_coord.z);
  }
  if (T0.w == 0.) {
    T0.w = alpha.w;
  }

  if (tex_info.y == 0u) {
    if (tex_info.z == 0u) {
      color.xyz = fragment_color.xyz * T0.xyz;
      color.w = fragment_color.w;
    } else {
      color.xyz = T0.xyz * 0.5;
      color.w = fragment_color.w;
    }
  } else {
    if (tex_info.z == 0u) {
      color = fragment_color * T0;
    } else {
      color.xyz = T0.xyz * 0.5;
      color.w = T0.w;
    }
  }
  color *= 2.;
  color.xyz *= mult.x;
  color.w *= mult.y;
  if (alpha.z > 0.5) {
    if (color.a <= alpha.x || color.a > alpha.y) discard;
  } else {
    if (color.a < alpha.x || color.a >= alpha.y) discard;
  }
  if (tex_info.w == 1u) {
    color.xyz = mix(color.xyz, fog_color.rgb, clamp(fog_color.a * fog, 0., 1.));
  }
  // GL blends (Cs-Cd)*FIX+Cd with glBlendColor; here StdAlpha + a constant output alpha.
  if (fix.x >= 0.) {
    color.w = fix.x;
  }
}
