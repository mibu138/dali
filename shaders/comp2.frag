#version 460
#extension GL_GOOGLE_include_directive : enable

#include "common.glsl"

layout(location = 0) in  vec2 inUv;

layout(location = 0) out vec4 outColor;

layout (input_attachment_index = 1, set = 1, binding = 0) uniform subpassInput inputC;
layout (input_attachment_index = 0, set = 1, binding = 1) uniform subpassInput inputB;
layout (input_attachment_index = 2, set = 1, binding = 2) uniform subpassInput inputD;

void main()
{
    vec4 color = subpassLoad(inputC);
    color = over(subpassLoad(inputB), color);
    color = over(subpassLoad(inputD), color);
    outColor = color;
}
