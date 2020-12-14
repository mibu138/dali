#version 460
#extension GL_GOOGLE_include_directive : enable

#include "common.glsl"

layout(location = 0) in  vec2 inUv;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 3) uniform sampler2D image;

void main()
{
    outColor = texture(image, inUv);
    //outColor = vec4(inUvw, 1);
}
