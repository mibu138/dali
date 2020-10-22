#version 460
#extension GL_EXT_ray_tracing : require

#include "selcommon.glsl"

layout(location = 0) rayPayloadInEXT hitPayload prd;

void main()
{
    prd.hit = false;
}
