#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_EXT_scalar_block_layout  : enable
#extension GL_GOOGLE_include_directive : enable

#include "raycommon.glsl"

layout(location = 0) rayPayloadInEXT hitPayload prd;

//layout(binding = 1, set = 0) buffer Attributes {
//    float a[];
//} attribs;
layout(binding = 1, set = 0) buffer Attributes {
    vec2 a[];
} attribs;

layout(binding = 2, set = 0) buffer Indices {
    uint i[];
} indices;

layout(binding = 0, set = 1) uniform accelerationStructureEXT topLevelAS;

layout(push_constant) uniform Constants {
    uint  posOffset;
    uint  normalOffset;
    uint  uvwOffset;
} pushC;

hitAttributeEXT vec3 hitAttrs;

layout(location = 1) rayPayloadEXT bool isShadowed;

void main()
{
    ivec3 ind = ivec3(
        indices.i[3 * gl_PrimitiveID + 0],
        indices.i[3 * gl_PrimitiveID + 1],
        indices.i[3 * gl_PrimitiveID + 2]);

    const vec3 barycen = vec3(1.0 - hitAttrs.x - hitAttrs.y, hitAttrs.x, hitAttrs.y);

    //const uint uvwOffset = pushC.uvwOffset;
    const uint uvwOffset = 21000;

    const vec3 uvw0 = vec3(attribs.a[ind[0] * 2 + uvwOffset], attribs.a[ind[0] * 2 + 1 + uvwOffset], 0);
    const vec3 uvw1 = vec3(attribs.a[ind[1] * 2 + uvwOffset], attribs.a[ind[1] * 2 + 1 + uvwOffset], 0);
    const vec3 uvw2 = vec3(attribs.a[ind[2] * 2 + uvwOffset], attribs.a[ind[2] * 2 + 1 + uvwOffset], 0);

    vec3 uvw    = uvw0 * barycen.x + uvw1 * barycen.y + uvw2 * barycen.z;

    prd.hitUv = uvw.xy;
}
