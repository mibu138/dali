#include "render.h"
#include "layer.h"
#include "tanto/r_geo.h"
#include "tanto/u_ui.h"
#include "tanto/v_image.h"
#include "tanto/v_memory.h"
#include <memory.h>
#include <assert.h>
#include <string.h>
#include <tanto/r_render.h>
#include <tanto/v_video.h>
#include <tanto/t_def.h>
#include <tanto/t_utils.h>
#include <tanto/r_pipeline.h>
#include <tanto/r_raytrace.h>
#include <tanto/v_command.h>
#include <tanto/r_renderpass.h>
#include <vulkan/vulkan_core.h>

#define SPVDIR "/home/michaelb/dev/painter/shaders/spv"

typedef Brush UboBrush;

typedef struct {
    Vec4 clearColor;
    Vec3 lightDir;
    float lightIntensity;
    int   lightType;
    uint32_t posOffset;
    uint32_t colorOffset;
    uint32_t normalOffset;
    uint32_t uvwOffset;
} RtPushConstants;

typedef struct {
    uint32_t index;
} RasterPushConstants;

static Tanto_V_BufferRegion  matrixRegion;
static Tanto_V_BufferRegion  brushRegion;
static Tanto_V_BufferRegion  stbPaintRegion;
static Tanto_V_BufferRegion  stbSelectRegion;
static Tanto_V_BufferRegion  playerRegion;
static Tanto_V_BufferRegion  selectionRegion;

static Tanto_R_Primitive     renderPrim;

static RtPushConstants     rtPushConstants;

static VkPipelineLayout pipelineLayouts[TANTO_MAX_PIPELINES];
static VkPipeline       graphicsPipelines[TANTO_MAX_PIPELINES];
static VkPipeline       raytracePipelines[TANTO_MAX_PIPELINES];

static VkDescriptorSetLayout descriptorSetLayouts[TANTO_MAX_DESCRIPTOR_SETS];
static Tanto_R_Description   description;

static Tanto_V_Image   depthAttachment;

static Tanto_V_Image   imageA;
static Tanto_V_Image   imageB;

static VkFramebuffer   frameBufferA;
static VkFramebuffer   frameBufferB;

static VkFramebuffer   swapchainFrameBuffers[TANTO_FRAME_COUNT];

static const VkFormat depthFormat   = VK_FORMAT_D32_SFLOAT;
static const VkFormat textureFormat = VK_FORMAT_R8G8B8A8_UNORM;

static VkRenderPass textureCompRenderPass;
static VkRenderPass swapchainRenderPass;

static uint8_t framesNeedUpdate;

static void updateRenderCommands(const int8_t frameIndex);
static void initOffscreenAttachments(void);
static void initCompRenderPass(void);
static void initSwapRenderPass(void);
static void initPaintAndTextureImage(void);
static void updatePrimDescriptors(void);
static void initNonMeshDescriptors(void);
static void initDescSetsAndPipeLayouts(void);
static void initPipelines(void);
static void rayTraceSelect(const VkCommandBuffer cmdBuf);
static void paint(const VkCommandBuffer cmdBuf);
static void rasterize(const VkCommandBuffer cmdBuf);
static void createShaderBindingTableSelect(void);
static void createShaderBindingTablePaint(void);
static void updatePushConstants(void);
static void initFramebuffers(void);
static void cleanUpSwapchainDependent(void);
static void onCreateLayer(void);
static void updateRenderCommands(const int8_t frameIndex);
static void onRecreateSwapchain(void);
VkDeviceSize r_GetTextureSize(void) { return imageA.size; }
void         r_InitRenderer(void);
void         r_Render(void);
int          r_GetSelectionPos(Vec3* v);
void         r_LoadPrim(Tanto_R_Primitive prim);
void         r_ClearPrim(void);
void         r_SavePaintImage(void);
void         r_ClearPaintImage(void);
void         r_CleanUp(void);
Mat4*        r_GetXform(r_XformType type);
Brush*       r_GetBrush(void);
UboPlayer*   r_GetPlayer(void);
void         r_SetPaintMode(const PaintMode mode);

#define TEXTURE_SIZE 0x1000 // 0x1000 = 4096

enum {
    LAYOUT_RASTER,
    LAYOUT_RAYTRACE,
    LAYOUT_POST,
};

enum {
    DESC_SET_RASTER,
    DESC_SET_RAYTRACE,
    DESC_SET_POST,
    DESC_SET_COMP
};

enum {
    PIPELINE_RASTER,
    PIPELINE_POST,
    PIPELINE_COMP,
};

enum {
    PIPELINE_RAY_TRACE,
    PIPELINE_SELECT,
};

typedef enum {
    IMAGE_A,
    IMAGE_B
} Image;

static void initOffscreenAttachments(void)
{
    depthAttachment = tanto_v_CreateImage(
            TANTO_WINDOW_WIDTH, TANTO_WINDOW_HEIGHT,
            depthFormat,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_ASPECT_DEPTH_BIT, 
            VK_SAMPLE_COUNT_1_BIT);
}

static void initCompRenderPass(void)
{
    tanto_r_CreateRenderPass_Color(VK_ATTACHMENT_LOAD_OP_LOAD, 
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 
            textureFormat, &textureCompRenderPass);
}

static void initSwapRenderPass(void)
{
    tanto_r_CreateRenderPass_ColorDepth(VK_ATTACHMENT_LOAD_OP_CLEAR, 
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 
            tanto_r_GetSwapFormat(), depthFormat, &swapchainRenderPass);
}

static void initPaintAndTextureImage(void)
{
    imageA = tanto_v_CreateImageAndSampler(TEXTURE_SIZE, TEXTURE_SIZE, textureFormat, 
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 
            VK_IMAGE_ASPECT_COLOR_BIT,
            VK_FILTER_LINEAR, 
            1);

    imageB = tanto_v_CreateImageAndSampler(TEXTURE_SIZE, TEXTURE_SIZE, textureFormat, 
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 
            VK_IMAGE_ASPECT_COLOR_BIT,
            VK_FILTER_LINEAR, 
            1);

    // must be transfer dst optimal to clear
    tanto_v_TransitionImageLayout(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, &imageA);
    tanto_v_TransitionImageLayout(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, &imageB);

    tanto_v_ClearColorImage(&imageA);
    tanto_v_ClearColorImage(&imageB);
}

static void updatePrimDescriptors(void)
{
    VkWriteDescriptorSetAccelerationStructureKHR asInfo = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
        .accelerationStructureCount = 1,
        .pAccelerationStructures    = &topLevelAS.handle
    };

    VkDescriptorBufferInfo vertBufInfo = {
        .offset = renderPrim.vertexRegion.offset,
        .range  = renderPrim.vertexRegion.size,
        .buffer = renderPrim.vertexRegion.buffer,
    };

    VkDescriptorBufferInfo indexBufInfo = {
        .offset = renderPrim.indexRegion.offset,
        .range  = renderPrim.indexRegion.size,
        .buffer = renderPrim.indexRegion.buffer,
    };

    VkWriteDescriptorSet writes[] = {{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstArrayElement = 0,
            .dstSet = description.descriptorSets[DESC_SET_RASTER],
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &vertBufInfo
        },{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstArrayElement = 0,
            .dstSet = description.descriptorSets[DESC_SET_RASTER],
            .dstBinding = 2,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &indexBufInfo
        },{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstArrayElement = 0,
            .dstSet = description.descriptorSets[DESC_SET_RAYTRACE],
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
            .pNext = &asInfo
    }};

    vkUpdateDescriptorSets(device, TANTO_ARRAY_SIZE(writes), writes, 0, NULL);
}

static void initNonMeshDescriptors(void)
{
    matrixRegion = tanto_v_RequestBufferRegion(sizeof(UboMatrices), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            TANTO_V_MEMORY_HOST_TYPE);
    UboMatrices* matrices = (UboMatrices*)matrixRegion.hostData;
    matrices->model   = m_Ident_Mat4();
    matrices->view    = m_Ident_Mat4();
    matrices->proj    = m_Ident_Mat4();
    matrices->viewInv = m_Ident_Mat4();
    matrices->projInv = m_Ident_Mat4();

    brushRegion = tanto_v_RequestBufferRegion(sizeof(UboBrush), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            TANTO_V_MEMORY_HOST_TYPE);
    UboBrush* brush = (UboBrush*)brushRegion.hostData;
    memset(brush, 0, sizeof(Brush));
    brush->radius = 0.01;
    brush->mode = 1;

    playerRegion = tanto_v_RequestBufferRegion(sizeof(UboPlayer), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, TANTO_V_MEMORY_HOST_TYPE);
    memset(playerRegion.hostData, 0, sizeof(UboPlayer));

    selectionRegion = tanto_v_RequestBufferRegion(sizeof(Selection), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, TANTO_V_MEMORY_HOST_TYPE);


    VkDescriptorBufferInfo uniformInfoMatrices = {
        .range  = matrixRegion.size,
        .offset = matrixRegion.offset,
        .buffer = matrixRegion.buffer,
    };

    VkDescriptorBufferInfo uniformInfoBrush = {
        .range  = brushRegion.size,
        .offset = brushRegion.offset,
        .buffer = brushRegion.buffer,
    };

    VkDescriptorBufferInfo uniformInfoPlayer = {
        .range  = playerRegion.size,
        .offset = playerRegion.offset,
        .buffer = playerRegion.buffer,
    };

    VkDescriptorBufferInfo storageBufInfoSelection= {
        .range  = selectionRegion.size,
        .offset = selectionRegion.offset,
        .buffer = selectionRegion.buffer,
    };

    VkDescriptorImageInfo imageInfoPaint = {
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .imageView   = imageA.view,
        .sampler     = imageA.sampler
    };

    VkDescriptorImageInfo imageInfoA = {
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .imageView   = imageA.view,
        .sampler     = imageA.sampler
    };

    VkDescriptorImageInfo imageInfoB = {
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .imageView   = imageB.view,
        .sampler     = imageB.sampler
    };

    VkWriteDescriptorSet writes[] = {{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstArrayElement = 0,
            .dstSet = description.descriptorSets[DESC_SET_RASTER],
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .pBufferInfo = &uniformInfoMatrices
        },{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstArrayElement = 0,
            .dstSet = description.descriptorSets[DESC_SET_RASTER],
            .dstBinding = 3,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &imageInfoPaint
        },{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstArrayElement = 0,
            .dstSet = description.descriptorSets[DESC_SET_RASTER],
            .dstBinding = 4,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .pBufferInfo = &uniformInfoPlayer
        },{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstArrayElement = 0,
            .dstSet = description.descriptorSets[DESC_SET_RASTER],
            .dstBinding = 5,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &imageInfoA
        },{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstArrayElement = 1,
            .dstSet = description.descriptorSets[DESC_SET_RASTER],
            .dstBinding = 5,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &imageInfoB
        },{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstArrayElement = 0,
            .dstSet = description.descriptorSets[DESC_SET_RAYTRACE],
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .pImageInfo = &imageInfoPaint
        },{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstArrayElement = 0,
            .dstSet = description.descriptorSets[DESC_SET_RAYTRACE],
            .dstBinding = 2,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &storageBufInfoSelection
        },{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstArrayElement = 0,
            .dstSet = description.descriptorSets[DESC_SET_POST],
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .pBufferInfo = &uniformInfoBrush,
    }};

    vkUpdateDescriptorSets(device, TANTO_ARRAY_SIZE(writes), writes, 0, NULL);
}

static void initDescSetsAndPipeLayouts(void)
{
    const Tanto_R_DescriptorSetInfo descSets[] = {{
            .bindingCount = 6, 
            .bindings = {{
                .descriptorCount = 1,
                .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR
            },{
                .descriptorCount = 1,
                .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .stageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR
            },{
                .descriptorCount = 1,
                .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .stageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR
            },{ // paint image
                .descriptorCount = 1,
                .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT
            },{
                .descriptorCount = 1,
                .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT
            },{ // texture images
                .descriptorCount = 2,
                .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT
            }}
        },{
            .bindingCount = 3,
            .bindings = {{
                .descriptorCount = 1,
                .type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
                .stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR
            },{
                .descriptorCount = 1,
                .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR
            },{
                .descriptorCount = 1,
                .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR
            }}
        },{
            .bindingCount = 1,
            .bindings = {{
                .descriptorCount = 1,
                .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR
            }}
        }
    };

    tanto_r_CreateDescriptorSetLayouts(TANTO_ARRAY_SIZE(descSets), descSets, descriptorSetLayouts);

    tanto_r_CreateDescriptorSets(TANTO_ARRAY_SIZE(descSets), descSets, descriptorSetLayouts, &description);

    const VkPushConstantRange pushConstantRt = {
        .stageFlags = 
            VK_SHADER_STAGE_RAYGEN_BIT_KHR |
            VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
            VK_SHADER_STAGE_MISS_BIT_KHR,
        .offset = 0,
        .size = sizeof(RtPushConstants)
    };

    const VkPushConstantRange pushConstantRaster = {
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(RtPushConstants)
    };

    const Tanto_R_PipelineLayoutInfo pipeLayoutInfos[] = {{
        .descriptorSetCount = 1, 
        .descriptorSetLayouts = descriptorSetLayouts,
        .pushConstantCount = 1,
        .pushConstantsRanges = &pushConstantRaster
    },{
        .descriptorSetCount = 3, 
        .descriptorSetLayouts = descriptorSetLayouts,
        .pushConstantCount = 1, 
        .pushConstantsRanges = &pushConstantRt,
    },{
        .descriptorSetCount = 1, 
        .descriptorSetLayouts = &descriptorSetLayouts[DESC_SET_POST]
    }};

    tanto_r_CreatePipelineLayouts(TANTO_ARRAY_SIZE(pipeLayoutInfos), pipeLayoutInfos, pipelineLayouts);
}

static void initPipelines(void)
{
    const Tanto_R_GraphicsPipelineInfo pipeInfosGraph[] = {{
        // raster
        .renderPass = swapchainRenderPass, 
        .layout     = pipelineLayouts[LAYOUT_RASTER],
        .vertexDescription = tanto_r_GetVertexDescription3D_3Vec3(),
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .sampleCount = VK_SAMPLE_COUNT_1_BIT,
        .vertShader = SPVDIR"/raster-vert.spv", 
        .fragShader = SPVDIR"/raster-frag.spv"
    },{
        // post
        .renderPass = swapchainRenderPass,
        .layout     = pipelineLayouts[LAYOUT_POST],
        .sampleCount = VK_SAMPLE_COUNT_1_BIT,
        .frontFace   = VK_FRONT_FACE_CLOCKWISE,
        .blendMode   = TANTO_R_BLEND_MODE_OVER,
        .vertShader = tanto_r_FullscreenTriVertShader(),
        .fragShader = SPVDIR"/post-frag.spv"
    }};

    const Tanto_R_RayTracePipelineInfo pipeInfosRT[] = {{
        // ray trace
        .layout = pipelineLayouts[LAYOUT_RAYTRACE],
        .raygenCount = 1,
        .raygenShaders = (char*[]){
            SPVDIR"/paint-rgen.spv",
        },
        .missCount = 1,
        .missShaders = (char*[]){
            SPVDIR"/paint-rmiss.spv",
        },
        .chitCount = 1,
        .chitShaders = (char*[]){
            SPVDIR"/paint-rchit.spv"
        }
    },{
        // select
        .layout = pipelineLayouts[LAYOUT_RAYTRACE],
        .raygenCount = 1,
        .raygenShaders = (char*[]){
            SPVDIR"/select-rgen.spv",
        },
        .missCount = 1,
        .missShaders = (char*[]){
            SPVDIR"/select-rmiss.spv",
        },
        .chitCount = 1,
        .chitShaders = (char*[]){
            SPVDIR"/select-rchit.spv"
        }
    }};

    tanto_r_CreateGraphicsPipelines(TANTO_ARRAY_SIZE(pipeInfosGraph), pipeInfosGraph, graphicsPipelines);
    tanto_r_CreateRayTracePipelines(TANTO_ARRAY_SIZE(pipeInfosRT), pipeInfosRT, raytracePipelines);
}

static void initCompPipeline(const Tanto_R_BlendMode blendMode)
{
    assert(blendMode != TANTO_R_BLEND_MODE_NONE);

    const Tanto_R_GraphicsPipelineInfo pipeInfo = {
        .layout  = pipelineLayouts[LAYOUT_RASTER],
        .renderPass = textureCompRenderPass, 
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .sampleCount = VK_SAMPLE_COUNT_1_BIT,
        .viewportDim = {TEXTURE_SIZE, TEXTURE_SIZE},
        .blendMode   = blendMode,
        .vertShader = tanto_r_FullscreenTriVertShader(),
        .fragShader = SPVDIR"/comp-frag.spv"
    };

    tanto_r_CreateGraphicsPipelines(1, &pipeInfo, &graphicsPipelines[PIPELINE_COMP]);
}

static void createShaderBindingTableSelect(void)
{
    const VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtprops = tanto_v_GetPhysicalDeviceRayTracingProperties();
    const uint32_t groupCount = 3;
    const uint32_t groupHandleSize = rtprops.shaderGroupHandleSize;
    const uint32_t baseAlignment = rtprops.shaderGroupBaseAlignment;
    const uint32_t sbtSize = groupCount * baseAlignment; // 3 shader groups: raygen, miss, closest hit

    uint8_t shaderHandleData[sbtSize];

    printf("ShaderGroup handle size: %d\n", groupHandleSize);
    printf("ShaderGroup base alignment: %d\n", baseAlignment);
    printf("ShaderGroups total size   : %d\n", sbtSize);

    VkResult r;
    r = vkGetRayTracingShaderGroupHandlesKHR(device, raytracePipelines[PIPELINE_SELECT], 0, groupCount, sbtSize, shaderHandleData);
    assert( VK_SUCCESS == r );
    stbSelectRegion = tanto_v_RequestBufferRegionAligned(sbtSize, baseAlignment, TANTO_V_MEMORY_HOST_TYPE);

    uint8_t* pSrc    = shaderHandleData;
    uint8_t* pTarget = stbSelectRegion.hostData;

    for (int i = 0; i < groupCount; i++) 
    {
        memcpy(pTarget, pSrc + i * groupHandleSize, groupHandleSize);
        pTarget += baseAlignment;
    }

    printf("Created shader binding table\n");
}

static void createShaderBindingTablePaint(void)
{
    const VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtprops = tanto_v_GetPhysicalDeviceRayTracingProperties();
    const uint32_t groupCount = 3;
    const uint32_t groupHandleSize = rtprops.shaderGroupHandleSize;
    const uint32_t baseAlignment = rtprops.shaderGroupBaseAlignment;
    const uint32_t sbtSize = groupCount * baseAlignment; // 3 shader groups: raygen, miss, closest hit

    uint8_t shaderHandleData[sbtSize];

    printf("ShaderGroup base alignment: %d\n", baseAlignment);
    printf("ShaderGroups total size   : %d\n", sbtSize);

    V_ASSERT( vkGetRayTracingShaderGroupHandlesKHR(device, raytracePipelines[PIPELINE_RAY_TRACE], 0, groupCount, sbtSize, shaderHandleData) );
    stbPaintRegion = tanto_v_RequestBufferRegionAligned(sbtSize, baseAlignment, TANTO_V_MEMORY_HOST_TYPE);

    uint8_t* pSrc    = shaderHandleData;
    uint8_t* pTarget = stbPaintRegion.hostData;

    for (int i = 0; i < groupCount; i++) 
    {
        memcpy(pTarget, pSrc + i * groupHandleSize, groupHandleSize);
        pTarget += baseAlignment;
    }

    printf("Created shader binding table\n");
}

static void updatePushConstants(void)
{
    rtPushConstants.clearColor = (Vec4){0.1, 0.2, 0.5, 1.0};
    rtPushConstants.lightIntensity = 1.0;
    rtPushConstants.lightDir = (Vec3){-0.707106769, -0.5, -0.5};
    rtPushConstants.lightType = 0;
    rtPushConstants.posOffset =    renderPrim.attrOffsets[0] / sizeof(Vec3);
    rtPushConstants.normalOffset = renderPrim.attrOffsets[1] / sizeof(Vec3);
    rtPushConstants.uvwOffset    = renderPrim.attrOffsets[2] / sizeof(Vec3);
}

static void initFramebuffers(void)
{
    for (int i = 0; i < TANTO_FRAME_COUNT; i++) 
    {
        Tanto_R_Frame* frame = tanto_r_GetFrame(i);

        const VkImageView offscreenAttachments[] = {frame->swapImage.view, depthAttachment.view};

        const VkFramebufferCreateInfo framebufferInfo = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .layers = 1,
            .height = TANTO_WINDOW_HEIGHT,
            .width  = TANTO_WINDOW_WIDTH,
            .renderPass = swapchainRenderPass,
            .attachmentCount = 2,
            .pAttachments = offscreenAttachments 
        };

        V_ASSERT( vkCreateFramebuffer(device, &framebufferInfo, NULL, &swapchainFrameBuffers[i]) );
    }

    VkFramebufferCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .layers = 1,
        .height = TEXTURE_SIZE,
        .width  = TEXTURE_SIZE,
        .renderPass = textureCompRenderPass,
        .attachmentCount = 1,
        .pAttachments = &imageA.view 
    };

    V_ASSERT( vkCreateFramebuffer(device, &info, NULL, &frameBufferA) );

    info.pAttachments = &imageB.view;

    V_ASSERT( vkCreateFramebuffer(device, &info, NULL, &frameBufferB) );
}

static void cleanUpSwapchainDependent(void)
{
    for (int i = 0; i < TANTO_FRAME_COUNT; i++) 
    {
        vkDestroyFramebuffer(device, swapchainFrameBuffers[i], NULL);
    }
    vkDestroyPipeline(device, graphicsPipelines[PIPELINE_RASTER], NULL);
    graphicsPipelines[PIPELINE_RASTER] = VK_NULL_HANDLE;
    vkDestroyPipeline(device, raytracePipelines[PIPELINE_RAY_TRACE], NULL);
    raytracePipelines[PIPELINE_RAY_TRACE] = VK_NULL_HANDLE;
    vkDestroyPipeline(device, raytracePipelines[PIPELINE_SELECT], NULL);
    raytracePipelines[PIPELINE_SELECT] = VK_NULL_HANDLE;
    vkDestroyPipeline(device, graphicsPipelines[PIPELINE_POST], NULL);
    graphicsPipelines[PIPELINE_POST] = VK_NULL_HANDLE;
    tanto_v_FreeImage(&depthAttachment);
}

static void onCreateLayer(void)
{
    vkDeviceWaitIdle(device);
    // TODO
    for (int i = 0; i < TANTO_FRAME_COUNT; i++) 
    {
        updateRenderCommands(i);
    }
}

static void onRecreateSwapchain(void)
{
    cleanUpSwapchainDependent();

    initOffscreenAttachments();
    initPipelines();
    initFramebuffers();

    framesNeedUpdate = TANTO_FRAME_COUNT;
}

static void rayTraceSelect(const VkCommandBuffer cmdBuf)
{
    vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, raytracePipelines[PIPELINE_SELECT]); 

    vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, 
            pipelineLayouts[LAYOUT_RAYTRACE], 0, 3, description.descriptorSets, 0, NULL);

    vkCmdPushConstants(cmdBuf, pipelineLayouts[LAYOUT_RAYTRACE], 
        VK_SHADER_STAGE_RAYGEN_BIT_KHR | 
        VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
        VK_SHADER_STAGE_MISS_BIT_KHR, 0, sizeof(RtPushConstants), &rtPushConstants);

    const VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtprops = tanto_v_GetPhysicalDeviceRayTracingProperties();
    const VkDeviceSize progSize = rtprops.shaderGroupBaseAlignment;
    const VkDeviceSize baseAlignment = stbSelectRegion.offset;
    const VkDeviceSize rayGenOffset   = baseAlignment + 0;
    const VkDeviceSize missOffset     = baseAlignment + 1u * progSize;
    const VkDeviceSize hitGroupOffset = baseAlignment + 2u * progSize; // have to jump over 1 miss shaders

    assert( rayGenOffset % rtprops.shaderGroupBaseAlignment == 0 );

    VkBufferDeviceAddressInfo addrInfo = {
        .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = stbSelectRegion.buffer,
    };

    VkDeviceAddress address = vkGetBufferDeviceAddress(device, &addrInfo);

    const VkStridedDeviceAddressRegionKHR raygenShaderBindingTable = {
        .deviceAddress = address + rayGenOffset,
        .size          = progSize,
        .stride        = progSize 
    };

    const VkStridedDeviceAddressRegionKHR missShaderBindingTable = {
        .deviceAddress = address + missOffset,
        .size          = progSize,
        .stride        = progSize
    };

    const VkStridedDeviceAddressRegionKHR hitShaderBindingTable = {
        .deviceAddress = address + hitGroupOffset,
        .size          = progSize,
        .stride        = progSize 
    };

    const VkStridedDeviceAddressRegionKHR callableShaderBindingTable = {
    };

    vkCmdTraceRaysKHR(cmdBuf, &raygenShaderBindingTable,
            &missShaderBindingTable, &hitShaderBindingTable,
            &callableShaderBindingTable, 1, 
            1, 1);
}

static void paint(const VkCommandBuffer cmdBuf)
{
    vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, raytracePipelines[PIPELINE_RAY_TRACE]);

    vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, 
            pipelineLayouts[LAYOUT_RAYTRACE], 0, 3, description.descriptorSets, 0, NULL);

    vkCmdPushConstants(cmdBuf, pipelineLayouts[LAYOUT_RAYTRACE], 
        VK_SHADER_STAGE_RAYGEN_BIT_KHR | 
        VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
        VK_SHADER_STAGE_MISS_BIT_KHR, 0, sizeof(RtPushConstants), &rtPushConstants);

    const VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtprops = tanto_v_GetPhysicalDeviceRayTracingProperties();
    const VkDeviceSize progSize = rtprops.shaderGroupBaseAlignment;
    const VkDeviceSize baseAlignment = stbPaintRegion.offset;
    const VkDeviceSize rayGenOffset   = baseAlignment + 0;
    const VkDeviceSize missOffset     = baseAlignment + 1u * progSize;
    const VkDeviceSize hitGroupOffset = baseAlignment + 2u * progSize; // have to jump over 1 miss shaders

    assert( rayGenOffset % rtprops.shaderGroupBaseAlignment == 0 );

    VkBufferDeviceAddressInfo addrInfo = {
        .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = stbPaintRegion.buffer,
    };

    VkDeviceAddress address = vkGetBufferDeviceAddress(device, &addrInfo);

    const VkStridedDeviceAddressRegionKHR raygenShaderBindingTable = {
        .deviceAddress = address + rayGenOffset,
        .size          = progSize,
        .stride        = progSize 
    };

    const VkStridedDeviceAddressRegionKHR missShaderBindingTable = {
        .deviceAddress = address + missOffset,
        .size          = progSize,
        .stride        = progSize
    };

    const VkStridedDeviceAddressRegionKHR hitShaderBindingTable = {
        .deviceAddress = address + hitGroupOffset,
        .size          = progSize,
        .stride        = progSize 
    };

    const VkStridedDeviceAddressRegionKHR callableShaderBindingTable = {
    };

    vkCmdTraceRaysKHR(cmdBuf, &raygenShaderBindingTable,
            &missShaderBindingTable, &hitShaderBindingTable,
            &callableShaderBindingTable, TEXTURE_SIZE, 
            TEXTURE_SIZE, 1);

    printf("Raytrace recorded!\n");
}

static void comp(const VkCommandBuffer cmdBuf, const Image destImage)
{
    VkClearValue clear = {0.9f, 0.003f, 0.009f, 1.0f};

    const VkRenderPassBeginInfo rpass = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .clearValueCount = 1,
        .pClearValues = &clear,
        .renderArea = {{0, 0}, {TEXTURE_SIZE, TEXTURE_SIZE}},
        .renderPass =  textureCompRenderPass,
        .framebuffer = destImage == IMAGE_A ? frameBufferA : frameBufferB,
    };

    vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipelines[PIPELINE_COMP]);

    vkCmdBindDescriptorSets(
        cmdBuf, 
        VK_PIPELINE_BIND_POINT_GRAPHICS, 
        pipelineLayouts[LAYOUT_RASTER], 
        0, 1, &description.descriptorSets[DESC_SET_RASTER], 
        0, NULL);

    const uint32_t textureIndex = (destImage + 1) % 2;

    vkCmdPushConstants(cmdBuf, pipelineLayouts[LAYOUT_RASTER], 
            VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(uint32_t), &textureIndex);

    vkCmdBeginRenderPass(cmdBuf, &rpass, VK_SUBPASS_CONTENTS_INLINE);

        vkCmdDraw(cmdBuf, 3, 1, 0, 0);

    vkCmdEndRenderPass(cmdBuf);
}

static void rasterize(const VkCommandBuffer cmdBuf)
{
    VkClearValue clearValueColor =     {0.002f, 0.003f, 0.009f, 1.0f};
    VkClearValue clearValueDepth = {1.0, 0};

    VkClearValue clears[] = {clearValueColor, clearValueDepth};

    const VkRenderPassBeginInfo rpass = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .clearValueCount = 2,
        .pClearValues = clears,
        .renderArea = {{0, 0}, {TANTO_WINDOW_WIDTH, TANTO_WINDOW_HEIGHT}},
        .renderPass =  swapchainRenderPass,
        .framebuffer = swapchainFrameBuffers[tanto_r_GetCurrentFrameIndex()]
    };

    vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipelines[PIPELINE_RASTER]);

    vkCmdBindDescriptorSets(
        cmdBuf, 
        VK_PIPELINE_BIND_POINT_GRAPHICS, 
        pipelineLayouts[LAYOUT_RASTER], 
        0, 1, &description.descriptorSets[DESC_SET_RASTER], 
        0, NULL);

    vkCmdBeginRenderPass(cmdBuf, &rpass, VK_SUBPASS_CONTENTS_INLINE);

        tanto_r_DrawPrim(cmdBuf, &renderPrim);

        vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipelines[PIPELINE_POST]);

        vkCmdBindDescriptorSets(
            cmdBuf, 
            VK_PIPELINE_BIND_POINT_GRAPHICS, 
            pipelineLayouts[LAYOUT_POST], 
            0, 1, &description.descriptorSets[DESC_SET_POST],
            0, NULL);

        vkCmdDraw(cmdBuf, 3, 1, 0, 0);

    vkCmdEndRenderPass(cmdBuf);
}

static void updateRenderCommands(const int8_t frameIndex)
{
    updatePushConstants();

    Tanto_R_Frame* frame = tanto_r_GetFrame(frameIndex);
    vkResetCommandPool(device, frame->command.commandPool, 0);
    VkCommandBufferBeginInfo cbbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};

    VkCommandBuffer cmdBuf = frame->command.commandBuffer; 

    V_ASSERT( vkBeginCommandBuffer(cmdBuf, &cbbi) );

    VkClearColorValue clearColor = {
        .float32[0] = 0,
        .float32[1] = 0,
        .float32[2] = 0,
        .float32[3] = 0,
    };

    VkImageSubresourceRange imageRange = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseArrayLayer = 0,
        .baseMipLevel = 0,
        .levelCount = 1,
        .layerCount = 1,
    };

    VkImageMemoryBarrier imgBarrier0 = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .image = imageA.handle,
        .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .subresourceRange = imageRange,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, 
    };

    vkCmdPipelineBarrier(cmdBuf, 
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 
            VK_PIPELINE_STAGE_TRANSFER_BIT, 
            0, 0, NULL, 
            0, NULL, 1, &imgBarrier0);

    vkCmdClearColorImage(cmdBuf, imageA.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1, &imageRange);

    VkImageMemoryBarrier imgBarrier1 = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .image = imageA.handle,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .subresourceRange = imageRange,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT
    };

    vkCmdPipelineBarrier(cmdBuf, 
            VK_PIPELINE_STAGE_TRANSFER_BIT, 
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 
            0, 0, NULL, 
            0, NULL, 1, &imgBarrier1);

    paint(cmdBuf);

    VkImageMemoryBarrier imgBarrier2 = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .image = imageA.handle,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .subresourceRange = imageRange,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT
    };

    vkCmdPipelineBarrier(cmdBuf, 
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 
            0, 0, NULL, 
            0, NULL, 1, &imgBarrier2);

    comp(cmdBuf, IMAGE_B);

    //VkImageMemoryBarrier imgBarrier3 = {
    //    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
    //    .image = imageB.handle,
    //    .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
    //    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    //    .subresourceRange = imageRange,
    //    .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
    //    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT
    //};
    
    //VkMemoryBarrier memBarrier3 = {
    //    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
    //    .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
    //    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT
    //};

    //vkCmdPipelineBarrier(cmdBuf, 
    //        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 
    //        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 
    //        0, 1, &memBarrier3, 
    //        0, NULL, 0, NULL);

    rasterize(cmdBuf);

    V_ASSERT( vkEndCommandBuffer(cmdBuf) );
}

void r_InitRenderer(void)
{
    framesNeedUpdate = TANTO_FRAME_COUNT;

    initOffscreenAttachments();
    initPaintAndTextureImage();

    assert(imageA.size > 0);
    l_Init(imageA.size); // eventually will move this out

    l_SetCreateLayerCallback(onCreateLayer);

    initSwapRenderPass();
    initCompRenderPass();
    initDescSetsAndPipeLayouts();
    initPipelines();
    initCompPipeline(TANTO_R_BLEND_MODE_OVER);

    createShaderBindingTablePaint();
    createShaderBindingTableSelect();
    initNonMeshDescriptors();

    initFramebuffers();

    tanto_r_RegisterSwapchainRecreationFn(onRecreateSwapchain);
}

void r_Render(void)
{
    if (framesNeedUpdate)
    {
        uint32_t i = tanto_r_GetCurrentFrameIndex();
        tanto_r_WaitOnFrame(i);
        updateRenderCommands(i);
        framesNeedUpdate--;
    }
    tanto_r_SubmitFrame();
}

int r_GetSelectionPos(Vec3* v)
{
    Tanto_V_CommandPool pool = tanto_v_RequestOneTimeUseCommand();

    rayTraceSelect(pool.buffer);

    tanto_v_SubmitOneTimeCommandAndWait(&pool, 0);

    Selection* sel = (Selection*)selectionRegion.hostData;
    if (sel->hit)
    {
        v->x[0] = sel->x;
        v->x[1] = sel->y;
        v->x[2] = sel->z;
        return 1;
    }
    else
        return 0;
}

void r_LoadPrim(Tanto_R_Primitive prim)
{
    renderPrim = prim;
    tanto_r_BuildBlas(&renderPrim);
    tanto_r_BuildTlas();

    updatePrimDescriptors();
}

void r_ClearPrim(void)
{
    tanto_r_RayTraceDestroyAccelStructs();
    tanto_r_FreePrim(&renderPrim);
}

void r_SavePaintImage(void)
{
    printf("Please enter a file name with extension.\n");
    char strbuf[32];
    fgets(strbuf, 32, stdin);
    uint8_t len = strlen(strbuf);
    if (len < 5)
    {
        printf("Filename too small. Must include extension.\n");
        return;
    }
    if (strbuf[len - 1] == '\n')  strbuf[--len] = '\0'; 
    const char* ext = strbuf + len - 3;
    TANTO_DEBUG_PRINT("%s", ext);
    Tanto_V_ImageFileType fileType;
    if (strncmp(ext, "png", 3) == 0) fileType = TANTO_V_IMAGE_FILE_TYPE_PNG;
    else if (strncmp(ext, "jpg", 3) == 0) fileType = TANTO_V_IMAGE_FILE_TYPE_JPG;
    else 
    {
        printf("Bad extension.\n");
        return;
    }
    tanto_v_SaveImage(&imageA, fileType, strbuf);
}

void r_ClearPaintImage(void)
{
    printf("called %s. need to reimplement.\n", __PRETTY_FUNCTION__);
    //tanto_v_ClearColorImage(&paintImage);
}

void r_CleanUp(void)
{
    cleanUpSwapchainDependent();
    for (int i = 0; i < TANTO_MAX_PIPELINES; i++) 
    {
        if (graphicsPipelines[i] != VK_NULL_HANDLE)
            vkDestroyPipeline(device, graphicsPipelines[i], NULL);
        if (raytracePipelines[i] != VK_NULL_HANDLE)
            vkDestroyPipeline(device, raytracePipelines[i], NULL);
    }
    memset(graphicsPipelines, 0, sizeof(graphicsPipelines));
    memset(raytracePipelines, 0, sizeof(raytracePipelines));
    tanto_v_FreeImage(&imageA);
    tanto_v_FreeImage(&imageB);
    vkDestroyDescriptorPool(device, description.descriptorPool, NULL);
    for (int i = 0; i < description.descriptorSetCount; i++) 
    {
        if (descriptorSetLayouts[i])
            vkDestroyDescriptorSetLayout(device, descriptorSetLayouts[i], NULL);
    }
    memset(&description, 0, sizeof(description));
    for (int i = 0; i < TANTO_MAX_PIPELINES; i++) 
    {
        if (pipelineLayouts[i])
            vkDestroyPipelineLayout(device, pipelineLayouts[i], NULL);
    }
    memset(&pipelineLayouts, 0, sizeof(pipelineLayouts));
    vkDestroyRenderPass(device, swapchainRenderPass, NULL);
    vkDestroyRenderPass(device, textureCompRenderPass, NULL);
    vkDestroyFramebuffer(device, frameBufferA, NULL);
    vkDestroyFramebuffer(device, frameBufferB, NULL);
    l_CleanUp();
}

Mat4* r_GetXform(r_XformType type)
{
    UboMatrices* matrices = (UboMatrices*)matrixRegion.hostData;
    switch (type) 
    {
        case R_XFORM_MODEL:    return &matrices->model;
        case R_XFORM_VIEW:     return &matrices->view;
        case R_XFORM_PROJ:     return &matrices->proj;
        case R_XFORM_VIEW_INV: return &matrices->viewInv;
        case R_XFORM_PROJ_INV: return &matrices->projInv;
    }
    return NULL;
}

Brush* r_GetBrush(void)
{
    assert (brushRegion.hostData);
    return (Brush*)brushRegion.hostData;
}

UboPlayer* r_GetPlayer(void)
{
    assert(playerRegion.hostData);
    return (UboPlayer*)playerRegion.hostData;
}

void r_SetPaintMode(const PaintMode mode)
{
    vkDeviceWaitIdle(device);
    vkDestroyPipeline(device, graphicsPipelines[PIPELINE_COMP], NULL);

    Tanto_R_BlendMode blendMode;
    switch (mode)
    {
        case PAINT_MODE_OVER:  blendMode = TANTO_R_BLEND_MODE_OVER;  break;
        case PAINT_MODE_ERASE: blendMode = TANTO_R_BLEND_MODE_ERASE; break;
    }

    initCompPipeline(blendMode);
    for (int i = 0; i < TANTO_FRAME_COUNT; i++) 
    {
        updateRenderCommands(i);
    }
}
