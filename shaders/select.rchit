#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_EXT_scalar_block_layout  : enable
#extension GL_GOOGLE_include_directive : enable

#include "selcommon.glsl"

layout(location = 0) rayPayloadInEXT hitPayload prd;

layout(set = 0, binding = 3, scalar) buffer Attributes {
    vec3 a[];
} attribs;

layout(set = 0, binding = 4) buffer Indices {
    uint i[];
} indices;

hitAttributeEXT vec3 hitAttrs;

layout(location = 1) rayPayloadEXT bool isShadowed;

void main()
{
    ivec3 ind = ivec3(
        indices.i[3 * gl_PrimitiveID + 0],
        indices.i[3 * gl_PrimitiveID + 1],
        indices.i[3 * gl_PrimitiveID + 2]);

    const vec3 barycen = vec3(1.0 - hitAttrs.x - hitAttrs.y, hitAttrs.x, hitAttrs.y);

    const vec3 p0 = attribs.a[ind[0]];
    const vec3 p1 = attribs.a[ind[1]];
    const vec3 p2 = attribs.a[ind[2]];

    vec3 pos    = p0 * barycen.x + p1 * barycen.y + p2 * barycen.z;

    prd.hit = true;
    prd.hitPos = pos;
}
