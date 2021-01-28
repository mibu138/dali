#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_EXT_scalar_block_layout  : enable
#extension GL_GOOGLE_include_directive : enable

#include "raycommon.glsl"

layout(location = 0) rayPayloadInEXT hitPayload prd;

layout(binding = 1, set = 0, scalar) buffer Attributes {
    vec3 a[];
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
    uint  colorOffset;
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

    const uint nOffset = pushC.normalOffset;
    const uint cOffset = pushC.colorOffset;
    const uint uvwOffset = pushC.uvwOffset;;

    const vec3 n0 = attribs.a[ind[0] + nOffset];
    const vec3 n1 = attribs.a[ind[1] + nOffset];
    const vec3 n2 = attribs.a[ind[2] + nOffset];

    const vec3 c0 = attribs.a[ind[0] + cOffset];
    const vec3 c1 = attribs.a[ind[1] + cOffset];
    const vec3 c2 = attribs.a[ind[2] + cOffset];

    const vec3 uvw0 = attribs.a[ind[0] + uvwOffset];
    const vec3 uvw1 = attribs.a[ind[1] + uvwOffset];
    const vec3 uvw2 = attribs.a[ind[2] + uvwOffset];

    vec3 normal = n0 * barycen.x + n1 * barycen.y + n2 * barycen.z;
    vec3 color  = c0 * barycen.x + c1 * barycen.y + c2 * barycen.z;
    vec3 uvw    = uvw0 * barycen.x + uvw1 * barycen.y + uvw2 * barycen.z;

    prd.hitUv = uvw.xy;
}
