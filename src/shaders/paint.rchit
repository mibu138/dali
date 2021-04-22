#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_EXT_scalar_block_layout  : enable
#extension GL_GOOGLE_include_directive : enable

#include "raycommon.glsl"

layout(location = 0) rayPayloadInEXT hitPayload prd;

layout(set = 0, binding = 0, scalar) buffer Uv {
    vec2 uv[];
} uvs;

layout(set = 0, binding = 1) buffer Indices {
    uint i[];
} indices;

hitAttributeEXT vec3 hitAttrs;

layout(location = 1) rayPayloadEXT bool isShadowed;

void main()
{
    const ivec3 ind = ivec3(
        indices.i[3 * gl_PrimitiveID + 0],
        indices.i[3 * gl_PrimitiveID + 1],
        indices.i[3 * gl_PrimitiveID + 2]);

    const vec3 barycen = vec3(1.0 - hitAttrs.x - hitAttrs.y, hitAttrs.x, hitAttrs.y);

    const vec2 uv0 = uvs.uv[ind[0]];
    const vec2 uv1 = uvs.uv[ind[1]];
    const vec2 uv2 = uvs.uv[ind[2]];

    const vec2 uv = uv0 * barycen.x + uv1 * barycen.y + uv2 * barycen.z;

    prd.hitUv = uv;
}
