#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_EXT_scalar_block_layout  : enable
#extension GL_GOOGLE_include_directive : enable

#include "raycommon.glsl"

layout(location = 0) rayPayloadInEXT hitPayload prd;

layout(binding = 1, set = 0, scalar) buffer Attributes {
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
    uint  uvOffset;
} pushC;

hitAttributeEXT vec3 hitAttrs;

layout(location = 1) rayPayloadEXT bool isShadowed;

vec2 getUv(const uint ind, const uint uvOffset)
{
    float u = attribs.a[ind * 2 + uvOffset];
    float v = attribs.a[ind * 2 + 1 + uvOffset];
    return vec2(u, v);
}

void main()
{
    ivec3 ind = ivec3(
        indices.i[3 * gl_PrimitiveID + 0],
        indices.i[3 * gl_PrimitiveID + 1],
        indices.i[3 * gl_PrimitiveID + 2]);

    const vec3 barycen = vec3(1.0 - hitAttrs.x - hitAttrs.y, hitAttrs.x, hitAttrs.y);

    const uint uvOffset  = pushC.uvOffset;

    const vec2 uv0 = getUv(ind[0], uvOffset);
    const vec2 uv1 = getUv(ind[1], uvOffset);
    const vec2 uv2 = getUv(ind[2], uvOffset);

    vec2 uv     = uv0 * barycen.x + uv1 * barycen.y + uv2 * barycen.z;

    prd.hitUv = uv;
}
