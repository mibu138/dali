#version 460
#extension GL_EXT_ray_tracing : require

#include "raycommon.glsl"

layout(location = 0) rayPayloadInEXT hitPayload hit;

void main()
{
    hit.uv = vec2(0.0, 0.0);
}
