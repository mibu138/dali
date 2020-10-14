#version 460
#extension GL_GOOGLE_include_directive : enable

#include "common.glsl"

layout(location = 0) in  vec3 inColor;
layout(location = 1) in  vec3 inNormal;
layout(location = 2) in  vec3 inUvw;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 3) uniform sampler2D image;

const vec3 lightDir = {-0.707106769, -0.5, -0.5};

void main()
{
    const float illume = clamp(dot(-1 * inNormal, lightDir), 0.0, 1.0);
    const vec4 inColor = vec4(inColor, 1.0);
    const vec4 tex = texture(image, inUvw.xy).rgba;
    const vec3 albedo = over(tex, inColor);
    outColor = vec4(albedo * illume, 1.0);
    //outColor = vec4(inUvw, 1);
}
