#version 310 es
precision highp float;
precision highp int;

out vec4 color;

layout(location = 0) in vec4 fragment_color;

void main() {
  color = fragment_color;
}
