#version 310 es
precision highp float;
precision highp int;

// PCRTC pass (AI-assisted): one full-screen triangle, no vertex buffer (gl_VertexID 0..2).
void main() {
  vec2 p = vec2(float((gl_VertexID & 1) * 4 - 1), float((gl_VertexID & 2) * 2 - 1));
  gl_Position = vec4(p, 0.0, 1.0);
}
