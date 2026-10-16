#version 460
// dk_probe bench fragment shader (deko3d migration P0.4, AI-assisted).
// Samples the texture bound for this draw (alternates per draw) times the UBO color.

layout (location = 0) in vec2 inUV;
layout (location = 0) out vec4 outColor;

layout (binding = 0) uniform sampler2D tex0;

layout (std140, binding = 0) uniform Params
{
    vec4 offset_scale;
    vec4 color;
} u;

void main()
{
    outColor = texture(tex0, inUV) * u.color;
}
