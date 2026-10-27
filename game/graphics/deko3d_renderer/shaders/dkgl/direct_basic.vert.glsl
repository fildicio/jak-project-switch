#version 310 es
precision highp float;
precision highp int;

layout (location = 0) in vec3 position_in;
layout (location = 1) in vec4 rgba_in;
layout (location = 5) in vec4 gs_scissor_in;

layout(location = 0) out vec4 fragment_color;
layout(location = 1) out vec4 gs_scissor;

void main() {
  // Note: position.y is multiplied by 32 instead of 16 to undo the half-height for interlacing stuff.
  gl_Position = vec4((position_in.x - 0.5) * 16., -(position_in.y - 0.5) * 32. * 1.0, position_in.z * 2. - 1., 1.0);
  // scissoring area adjust
  gl_Position.y *= (512.0 / 448.0);
  fragment_color = vec4(rgba_in.x, rgba_in.y, rgba_in.z, rgba_in.w * 2.);
  gs_scissor = gs_scissor_in;
}
