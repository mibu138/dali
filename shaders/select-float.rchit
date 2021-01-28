#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_EXT_scalar_block_layout  : enable
#extension GL_GOOGLE_include_directive : enable

#include "selcommon.glsl"

layout(location = 0) rayPayloadInEXT hitPayload prd;

layout(binding = 1, set = 0) buffer Attributes {
    float a[];
} attribs;

layout(binding = 2, set = 0) buffer Indices {
    uint i[];
} indices;

layout(binding = 0, set = 1) uniform accelerationStructureEXT topLevelAS;

layout(push_constant) uniform Constants {
    vec4 clearColor;
    vec3 lightDir;
    float lightIntensity;
    int   lightType;
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

    const uint pOffset = pushC.posOffset;

    const vec3 p0 = vec3(attribs.a[ind[0] * 3 + 0 + pOffset],
                         attribs.a[ind[0] * 3 + 1 + pOffset],
                         attribs.a[ind[0] * 3 + 2 + pOffset]);
    const vec3 p1 = vec3(attribs.a[ind[1] * 3 + 0 + pOffset],
                         attribs.a[ind[1] * 3 + 1 + pOffset],
                         attribs.a[ind[1] * 3 + 2 + pOffset]);
    const vec3 p2 = vec3(attribs.a[ind[2] * 3 + 0 + pOffset],
                         attribs.a[ind[2] * 3 + 1 + pOffset],
                         attribs.a[ind[2] * 3 + 2 + pOffset]);

    vec3 pos    = p0 * barycen.x + p1 * barycen.y + p2 * barycen.z;

    prd.hit = true;
    prd.hitPos = pos;
}
