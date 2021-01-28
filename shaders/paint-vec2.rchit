#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_EXT_scalar_block_layout  : enable
#extension GL_GOOGLE_include_directive : enable

#include "raycommon.glsl"

layout(location = 0) rayPayloadInEXT hitPayload prd;

layout(binding = 2, set = 0) buffer Indices {
    uint i[];
} indices;

layout(binding = 0, set = 1) uniform accelerationStructureEXT topLevelAS;

layout(binding = 3, set = 1, scalar) buffer Attributes {
    vec2 a[];
} attribs;

hitAttributeEXT vec3 hitAttrs;

layout(location = 1) rayPayloadEXT bool isShadowed;

void main()
{
    ivec3 ind = ivec3(
        indices.i[3 * gl_PrimitiveID + 0],
        indices.i[3 * gl_PrimitiveID + 1],
        indices.i[3 * gl_PrimitiveID + 2]);

    const vec3 barycen = vec3(1.0 - hitAttrs.x - hitAttrs.y, hitAttrs.x, hitAttrs.y);

    const vec3 uvw0 = vec3(attribs.a[ind[0]], 0);
    const vec3 uvw1 = vec3(attribs.a[ind[1]], 0);
    const vec3 uvw2 = vec3(attribs.a[ind[2]], 0);

    vec3 uvw    = uvw0 * barycen.x + uvw1 * barycen.y + uvw2 * barycen.z;

    prd.hitUv = uvw.xy;
}
