#version 310 es
precision highp float;
precision highp int;

layout (location = 0) in vec2 position_in;

void main() {
  gl_Position = vec4(position_in, 0, 1.0);
}
