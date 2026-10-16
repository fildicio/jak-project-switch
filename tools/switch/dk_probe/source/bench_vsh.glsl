#version 460
// dk_probe bench vertex shader (deko3d migration P0.4, AI-assisted).
// One small triangle per draw, placed by a std140 UBO that is rewritten with
// dkCmdBufPushConstants before every draw -- the deko3d analogue of the
// glUniform* + draw pattern the GL renderers use today.

layout (std140, binding = 0) uniform Params
{
    vec4 offset_scale;  // xy = clip-space offset, zw = scale
    vec4 color;
} u;

layout (location = 0) out vec2 outUV;

const vec2 positions[3] = vec2[](
    vec2( 0.0, +1.0),
    vec2(-1.0, -1.0),
    vec2(+1.0, -1.0)
);

void main()
{
    vec2 p = positions[gl_VertexID];
    gl_Position = vec4(p * u.offset_scale.zw + u.offset_scale.xy, 0.0, 1.0);
    outUV = p * 0.5 + 0.5;
}
