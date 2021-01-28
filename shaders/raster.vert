#version 460

layout(location = 0) in vec3 pos;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec2 uvw;

layout(location = 0) out vec3 outColor;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec3 outUvw;

layout(set = 0, binding = 0) uniform Matrices {
    mat4 model;
    mat4 view;
    mat4 proj;
    mat4 viewInv;
    mat4 projInv;
} matrices;

void main()
{
    gl_Position = matrices.proj * matrices.view * matrices.model * vec4(pos, 1.0);
    outColor = vec3(1, 1, 1);
    outNormal = normal;
    outUvw = vec3(uvw, 0);
}
