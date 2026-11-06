#version 310 es
precision highp float;
precision highp int;
layout(std140, binding = 7) uniform VertParams {
  vec3 positions[4];
  vec2 uvs[4];
  vec4 rgba;
};

layout (location = 0) in int vertex_index;




// TODO flags and stuff

layout(location = 0) out vec2 uv;

void main() {
  gl_Position = vec4(-1. + (positions[vertex_index] * 2.), 1.);
  uv = uvs[vertex_index];
}
