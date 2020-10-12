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
    uint  colorOffset;
    uint  normalOffset;
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

    const vec3 n0 = attribs.a[ind[0] + nOffset];
    const vec3 n1 = attribs.a[ind[1] + nOffset];
    const vec3 n2 = attribs.a[ind[2] + nOffset];

    const vec3 c0 = attribs.a[ind[0] + cOffset];
    const vec3 c1 = attribs.a[ind[1] + cOffset];
    const vec3 c2 = attribs.a[ind[2] + cOffset];

    vec3 normal = n0 * barycen.x + n1 * barycen.y + n2 * barycen.z;
    vec3 color  = c0 * barycen.x + c1 * barycen.y + c2 * barycen.z;

    const vec3 lightDir = pushC.lightDir;

    float illume = clamp(dot(-1 * normal, lightDir), 0.0, 1.0);

    isShadowed = true;

    if (illume > 0)
    {    
        float tMin   = 0.001;
        float tMax   = 100;
        vec3  origin = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * gl_HitTEXT;
        vec3  rayDir = lightDir * -1;
        uint  flags =
            gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT | gl_RayFlagsSkipClosestHitShaderEXT;
        isShadowed = true;
        traceRayEXT(topLevelAS,  // acceleration structure
                flags,       // rayFlags
                0xFF,        // cullMask
                0,           // sbtRecordOffset
                0,           // sbtRecordStride
                1,           // missIndex
                origin,      // ray origin
                tMin,        // ray min range
                rayDir,      // ray direction
                tMax,        // ray max range
                1            // payload (location = 1)
                );
    }

    if (isShadowed)
        illume = 0.0;

    prd.hitValue = color * illume;
}
