#version 460

layout(location = 0) in  vec3 inColor;
layout(location = 1) in  vec3 inNormal;

layout(location = 0) out vec4 outColor;

const vec3 lightDir = {-0.707106769, -0.5, -0.5};

void main()
{
    float illume = clamp(dot(-1 * inNormal, lightDir), 0.0, 1.0);
    outColor = vec4(inColor * illume, 1);
}
