#version 460

layout(location = 0) in  vec3 pos;
layout(location = 0) out vec3 outColor;

void main()
{
    gl_Position = vec4(pos, 1.0);
    outColor = vec3(0.1, 0.6, 1);
}
