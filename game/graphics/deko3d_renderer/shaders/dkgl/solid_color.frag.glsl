#version 310 es
precision highp float;
precision highp int;
layout(std140, binding = 7) uniform FragParams {
  vec4 fragment_color;
};

out vec4 color;



void main() {
  color = fragment_color;
}
