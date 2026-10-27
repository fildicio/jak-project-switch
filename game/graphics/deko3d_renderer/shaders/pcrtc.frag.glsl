#version 310 es
precision highp float;
precision highp int;

// PCRTC pass (AI-assisted): the GL path's final blit computes
//   rgb = image.rgb * alpha + add   (post_processing.frag, OpenGLRenderer::do_pcrtc_effects)
// The dk path draws straight into the swapchain image, so the same result comes from the
// blend unit (dst * blend-constant alpha + this colour); this shader only outputs |add|.
layout(std140, binding = 0) uniform Pcrtc {
  vec4 add_color;
};
layout(location = 0) out vec4 out_color;

void main() {
  out_color = add_color;
}
