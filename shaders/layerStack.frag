#version 460
#extension GL_GOOGLE_include_directive : enable
// allows for unbounded array
#extension GL_EXT_nonuniform_qualifier : enable 

#include "common.glsl"

layout(location = 0) in  vec2 inUv;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 6) uniform sampler2D image[];

layout(push_constant) uniform PER_OBJECT
{
	uint imgIdx;
} pc;

void main()
{
    outColor = texture(image[pc.imgIdx], inUv);
}
