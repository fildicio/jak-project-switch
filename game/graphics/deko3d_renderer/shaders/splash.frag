#version 460
// deko3d port of opengl_renderer/shaders/splash.* (AI-assisted)
layout(location = 0) in vec2 frag_uv;
layout(location = 0) out vec4 out_color;
layout(binding = 0) uniform sampler2D splash_tex;

void main() {
    out_color = texture(splash_tex, frag_uv);
}
