#version 310 es
precision highp float;
precision highp int;

in vec4 color;
out vec4 out_color;

void main() {
    out_color = color;
}