#version 460
#extension GL_GOOGLE_include_directive : enable

#include "common.glsl"

layout(location = 0) in  vec3 inColor;
layout(location = 1) in  vec3 inNormal;
layout(location = 2) in  vec3 inUvw;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 2) uniform sampler2D image;

void main()
{
    const vec3 lightDir = vec3(0, 0, -1);
    const float illume = clamp(dot(-1 * inNormal, lightDir), 0.0, 1.0);
    const ivec2 texel = ivec2(inUvw.xy * 50);
    const float b = float((texel.x + texel.y) % 2) * 0.2 + 0.3;
    const vec4 uvColor = vec4(b, b, b, 1.0);
    const vec4 inColor = vec4(inColor, 1.0);
    vec4 tex = texture(image, inUvw.xy);
    const vec4 albedo = over(tex, uvColor);
    outColor = vec4(albedo.rgb * illume, 1.0);
}
