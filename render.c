#include "render.h"
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
#include <vulkan/vulkan_core.h>

#define SPVDIR "/home/michaelb/dev/painter/shaders/spv"

typedef struct {
    Mat4 model;
    Mat4 view;
    Mat4 proj;
    Mat4 viewInv;
    Mat4 projInv;
} UboMatrices;

typedef Brush UboBrush;

static Tanto_V_BufferRegion  matrixBlock;
static Tanto_V_BufferRegion  brushBlock;
static Tanto_V_BufferRegion  stbBlock;
static Tanto_V_BufferRegion  playerBlock;

static Tanto_R_Mesh          hapiMesh;

static RtPushConstants pushConstants;

static Tanto_R_FrameBuffer  offscreenFrameBuffer;
static Tanto_V_Image        paintImage;
static Vec2                 paintImageDim;
static Vec2                 brushDim;

#define PAINT_IMG_SIZE 0x2000 // 0x1000 = 4096
#define BRUSH_IMG_SIZE 0x0800

typedef enum {
    R_PIPE_RASTER,
    R_PIPE_RAYTRACE,
    R_PIPE_POST,
    R_PIPE_ID_SIZE
} R_PipelineId;

typedef enum {
    R_PIPE_LAYOUT_RASTER,
    R_PIPE_LAYOUT_RAYTRACE,
    R_PIPE_LAYOUT_POST,
    R_PIPE_LAYOUT_ID_SIZE
} R_PipelineLayoutId;

typedef enum {
    R_DESC_SET_RASTER,
    R_DESC_SET_RAYTRACE,
    R_DESC_SET_POST,
    R_DESC_SET_ID_SIZE
} R_DescriptorSetId;

static void initOffscreenFrameBuffer(void)
{
    //initDepthAttachment();
    offscreenFrameBuffer.depthAttachment = tanto_v_CreateImage(
            TANTO_WINDOW_WIDTH, TANTO_WINDOW_HEIGHT,
            depthFormat,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_ASPECT_DEPTH_BIT);

    offscreenFrameBuffer.colorAttachment = tanto_v_CreateImageAndSampler(
            TANTO_WINDOW_WIDTH, TANTO_WINDOW_HEIGHT, offscreenColorFormat,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | 
            VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_STORAGE_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT,
            VK_FILTER_NEAREST);
    //
    // seting render pass and depth attachment
    offscreenFrameBuffer.pRenderPass = &offscreenRenderPass;

    const VkImageView attachments[] = {offscreenFrameBuffer.colorAttachment.view, offscreenFrameBuffer.depthAttachment.view};

    VkFramebufferCreateInfo framebufferInfo = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .layers = 1,
        .height = TANTO_WINDOW_HEIGHT,
        .width  = TANTO_WINDOW_WIDTH,
        .renderPass = *offscreenFrameBuffer.pRenderPass,
        .attachmentCount = 2,
        .pAttachments = attachments
    };

    V_ASSERT( vkCreateFramebuffer(device, &framebufferInfo, NULL, &offscreenFrameBuffer.handle) );

    tanto_v_TransitionImageLayout(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, &offscreenFrameBuffer.colorAttachment);
}

static void initPaintImage(void)
{
    paintImageDim.x = PAINT_IMG_SIZE;
    paintImageDim.y = PAINT_IMG_SIZE;
    paintImage = tanto_v_CreateImageAndSampler(paintImageDim.x, paintImageDim.y, offscreenColorFormat, 
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 
            VK_IMAGE_ASPECT_COLOR_BIT,
            VK_FILTER_LINEAR);

    tanto_v_TransitionImageLayout(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, &paintImage);
}

static void updateMeshDescriptors(void)
{
    VkWriteDescriptorSetAccelerationStructureKHR asInfo = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
        .accelerationStructureCount = 1,
        .pAccelerationStructures    = &topLevelAS
    };

    VkDescriptorBufferInfo vertBufInfo = {
        .offset = hapiMesh.vertexBlock.offset,
        .range  = hapiMesh.vertexBlock.size,
        .buffer = hapiMesh.vertexBlock.buffer,
    };

    VkDescriptorBufferInfo indexBufInfo = {
        .offset = hapiMesh.indexBlock.offset,
        .range  = hapiMesh.indexBlock.size,
        .buffer = hapiMesh.indexBlock.buffer,
    };

    VkWriteDescriptorSet writes[] = {{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstArrayElement = 0,
            .dstSet = descriptorSets[R_DESC_SET_RASTER],
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &vertBufInfo
        },{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstArrayElement = 0,
            .dstSet = descriptorSets[R_DESC_SET_RASTER],
            .dstBinding = 2,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &indexBufInfo
        },{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstArrayElement = 0,
            .dstSet = descriptorSets[R_DESC_SET_RAYTRACE],
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
            .pNext = &asInfo
    }};

    vkUpdateDescriptorSets(device, TANTO_ARRAY_SIZE(writes), writes, 0, NULL);
}

static void initNonMeshDescriptors(void)
{
    matrixBlock = tanto_v_RequestBufferRegion(sizeof(UboMatrices), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            TANTO_V_MEMORY_HOST_GRAPHICS_TYPE);
    UboMatrices* matrices = (UboMatrices*)matrixBlock.hostData;
    matrices->model   = m_Ident_Mat4();
    matrices->view    = m_Ident_Mat4();
    matrices->proj    = m_Ident_Mat4();
    matrices->viewInv = m_Ident_Mat4();
    matrices->projInv = m_Ident_Mat4();

    brushBlock = tanto_v_RequestBufferRegion(sizeof(UboBrush), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            TANTO_V_MEMORY_HOST_GRAPHICS_TYPE);
    UboBrush* brush = (UboBrush*)brushBlock.hostData;
    memset(brush, 0, sizeof(Brush));
    brush->radius = 0.01;
    brush->mode = 1;

    playerBlock = tanto_v_RequestBufferRegion(sizeof(UboPlayer), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, TANTO_V_MEMORY_HOST_GRAPHICS_TYPE);
    memset(playerBlock.hostData, 0, sizeof(UboPlayer));

    VkDescriptorBufferInfo uniformInfoMatrices = {
        .range  = matrixBlock.size,
        .offset = matrixBlock.offset,
        .buffer = matrixBlock.buffer,
    };

    VkDescriptorBufferInfo uniformInfoBrush = {
        .range  = brushBlock.size,
        .offset = brushBlock.offset,
        .buffer = brushBlock.buffer,
    };

    VkDescriptorBufferInfo uniformInfoPlayer = {
        .range  = playerBlock.size,
        .offset = playerBlock.offset,
        .buffer = playerBlock.buffer,
    };

    VkDescriptorImageInfo imageInfoPaint = {
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .imageView   = paintImage.view,
        .sampler     = paintImage.sampler
    };

    VkDescriptorImageInfo imageInfoOffscreenFBO = {
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .imageView   = offscreenFrameBuffer.colorAttachment.view,
        .sampler     = offscreenFrameBuffer.colorAttachment.sampler
    };

    VkWriteDescriptorSet writes[] = {{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstArrayElement = 0,
            .dstSet = descriptorSets[R_DESC_SET_RASTER],
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .pBufferInfo = &uniformInfoMatrices
        },{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstArrayElement = 0,
            .dstSet = descriptorSets[R_DESC_SET_RASTER],
            .dstBinding = 3,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &imageInfoPaint
        },{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstArrayElement = 0,
            .dstSet = descriptorSets[R_DESC_SET_RASTER],
            .dstBinding = 4,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .pBufferInfo = &uniformInfoPlayer
        },{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstArrayElement = 0,
            .dstSet = descriptorSets[R_DESC_SET_RAYTRACE],
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .pImageInfo = &imageInfoPaint
        },{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstArrayElement = 0,
            .dstSet = descriptorSets[R_DESC_SET_POST],
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &imageInfoOffscreenFBO
        },{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstArrayElement = 0,
            .dstSet = descriptorSets[R_DESC_SET_POST],
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .pBufferInfo = &uniformInfoBrush,
    }};

    vkUpdateDescriptorSets(device, TANTO_ARRAY_SIZE(writes), writes, 0, NULL);
}

static void InitPipelines(void)
{
    const Tanto_R_DescriptorSet descSets[] = {{
            .id = R_DESC_SET_RASTER,
            .bindingCount = 5, 
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
            },{
                .descriptorCount = 1,
                .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT
            },{
                .descriptorCount = 1,
                .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT
            }}
        },{
            .id = R_DESC_SET_RAYTRACE,
            .bindingCount = 2,
            .bindings = {{
                .descriptorCount = 1,
                .type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
                .stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR
            },{
                .descriptorCount = 1,
                .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR
            }}
        },{
            .id = R_DESC_SET_POST,
            .bindingCount = 2,
            .bindings = {{
                .descriptorCount = 1,
                .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT
            },{
                .descriptorCount = 1,
                .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR
            }}
        }
    };

    const VkPushConstantRange pushConstantRt = {
        .stageFlags = 
            VK_SHADER_STAGE_RAYGEN_BIT_KHR |
            VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
            VK_SHADER_STAGE_MISS_BIT_KHR,
        .offset = 0,
        .size = sizeof(RtPushConstants)
    };

    const Tanto_R_PipelineLayout pipelayouts[] = {{
        .id = R_PIPE_LAYOUT_RASTER, 
        .descriptorSetCount = 1, 
        .descriptorSetIds = {R_DESC_SET_RASTER}
    },{
        .id = R_PIPE_LAYOUT_RAYTRACE, 
        .descriptorSetCount = 3, 
        .descriptorSetIds = {R_DESC_SET_RASTER, R_DESC_SET_RAYTRACE, R_DESC_SET_POST}, 
        .pushConstantCount = 1, 
        .pushConstantsRanges = {pushConstantRt}
    },{
        .id = R_PIPE_LAYOUT_POST, 
        .descriptorSetCount = 1, 
        .descriptorSetIds = {R_DESC_SET_POST}
    }};

    const Tanto_R_PipelineInfo pipeInfos[] = {{
        .id       = R_PIPE_RASTER,
        .type     = TANTO_R_PIPELINE_RASTER_TYPE,
        .layoutId = R_PIPE_LAYOUT_RASTER,
        .payload.rasterInfo = {
            .renderPassType = TANTO_R_RENDER_PASS_OFFSCREEN_TYPE, 
            .vertShader = SPVDIR"/raster-vert.spv", 
            .fragShader = SPVDIR"/raster-frag.spv"
        }
    },{
        .id       = R_PIPE_RAYTRACE,
        .type     = TANTO_R_PIPELINE_RAYTRACE_TYPE,
        .layoutId = R_PIPE_LAYOUT_RAYTRACE,
        .payload.rayTraceInfo = {
            .raygenCount = 1,
            .raygenShaders = (char*[]){
                SPVDIR"/paint-rgen.spv",
            },
            .missCount = 2,
            .missShaders = (char*[]){
                SPVDIR"/paint-rmiss.spv",
                SPVDIR"/raytraceShadow-rmiss.spv"
            },
            .chitCount = 1,
            .chitShaders = (char*[]){
                SPVDIR"/paint-rchit.spv"
            }
        }
    },{
        .id       = R_PIPE_POST,
        .type     = TANTO_R_PIPELINE_POSTPROC_TYPE,
        .layoutId = R_PIPE_LAYOUT_POST,
        .payload.rasterInfo = {
            .renderPassType = TANTO_R_RENDER_PASS_SWAPCHAIN_TYPE,
            .vertShader = "",
            .fragShader = SPVDIR"/post-frag.spv"
        }
    }};

    tanto_r_InitDescriptorSets(descSets, TANTO_ARRAY_SIZE(descSets));
    tanto_r_InitPipelineLayouts(pipelayouts, TANTO_ARRAY_SIZE(pipelayouts));
    tanto_r_InitPipelines(pipeInfos, TANTO_ARRAY_SIZE(pipeInfos));
}

static void rayTrace(const VkCommandBuffer* cmdBuf)
{
    vkCmdBindPipeline(*cmdBuf, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipelines[R_PIPE_RAYTRACE]);

    vkCmdBindDescriptorSets(*cmdBuf, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, 
            pipelineLayouts[R_PIPE_LAYOUT_RAYTRACE], 0, 3, descriptorSets, 0, NULL);

    vkCmdPushConstants(*cmdBuf, pipelineLayouts[R_PIPE_LAYOUT_RAYTRACE], 
        VK_SHADER_STAGE_RAYGEN_BIT_KHR | 
        VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
        VK_SHADER_STAGE_MISS_BIT_KHR, 0, sizeof(RtPushConstants), &pushConstants);

    const VkPhysicalDeviceRayTracingPropertiesKHR rtprops = tanto_v_GetPhysicalDeviceRayTracingProperties();
    const VkDeviceSize progSize = rtprops.shaderGroupBaseAlignment;
    const VkDeviceSize sbtSize = rtprops.shaderGroupBaseAlignment * 4;
    const VkDeviceSize baseAlignment = stbBlock.offset;
    const VkDeviceSize rayGenOffset   = baseAlignment;
    const VkDeviceSize missOffset     = baseAlignment + 1u * progSize;
    const VkDeviceSize hitGroupOffset = baseAlignment + 3u * progSize; // have to jump over 2 miss shaders

    printf("Prog\n");

    assert( rayGenOffset % rtprops.shaderGroupBaseAlignment == 0 );

    const VkStridedBufferRegionKHR raygenShaderBindingTable = {
        .buffer = stbBlock.buffer,
        .offset = rayGenOffset,
        .stride = progSize,
        .size   = sbtSize,
    };

    const VkStridedBufferRegionKHR missShaderBindingTable = {
        .buffer = stbBlock.buffer,
        .offset = missOffset,
        .stride = progSize,
        .size   = sbtSize,
    };

    const VkStridedBufferRegionKHR hitShaderBindingTable = {
        .buffer = stbBlock.buffer,
        .offset = hitGroupOffset,
        .stride = progSize,
        .size   = sbtSize,
    };

    const VkStridedBufferRegionKHR callableShaderBindingTable = {
    };

    vkCmdTraceRaysKHR(*cmdBuf, &raygenShaderBindingTable,
            &missShaderBindingTable, &hitShaderBindingTable,
            &callableShaderBindingTable, brushDim.x, 
            brushDim.y, 1);

    printf("Raytrace recorded!\n");
}

static void rasterize(const VkCommandBuffer* cmdBuf, const VkRenderPassBeginInfo* rpassInfo)
{
    vkCmdBindPipeline(*cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[R_PIPE_RASTER]);

    vkCmdBindDescriptorSets(
        *cmdBuf, 
        VK_PIPELINE_BIND_POINT_GRAPHICS, 
        pipelineLayouts[R_PIPE_LAYOUT_RASTER], 
        0, 1, &descriptorSets[R_DESC_SET_RASTER], 
        0, NULL);

    vkCmdBeginRenderPass(*cmdBuf, rpassInfo, VK_SUBPASS_CONTENTS_INLINE);

        VkBuffer vertBuffers[4] = {
            hapiMesh.vertexBlock.buffer,
            hapiMesh.vertexBlock.buffer,
            hapiMesh.vertexBlock.buffer,
            hapiMesh.vertexBlock.buffer
        };

        const VkDeviceSize vertOffsets[4] = {
            hapiMesh.posOffset + hapiMesh.vertexBlock.offset, 
            hapiMesh.colOffset + hapiMesh.vertexBlock.offset,
            hapiMesh.norOffset + hapiMesh.vertexBlock.offset,
            hapiMesh.uvwOffset + hapiMesh.vertexBlock.offset
        };

        vkCmdBindVertexBuffers(
            *cmdBuf, 0, TANTO_ARRAY_SIZE(vertOffsets), 
            vertBuffers, vertOffsets);

        vkCmdBindIndexBuffer(
            *cmdBuf, 
            hapiMesh.indexBlock.buffer, 
            hapiMesh.indexBlock.offset, TANTO_VERT_INDEX_TYPE);

        vkCmdDrawIndexed(*cmdBuf, 
            hapiMesh.indexCount, 1, 0, 
            0, 0);

    vkCmdEndRenderPass(*cmdBuf);
}

static void postProc(const VkCommandBuffer* cmdBuf, const VkRenderPassBeginInfo* rpassInfo)
{
    vkCmdBindPipeline(*cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[R_PIPE_POST]);

    vkCmdBindDescriptorSets(
        *cmdBuf, 
        VK_PIPELINE_BIND_POINT_GRAPHICS, 
        pipelineLayouts[R_PIPE_LAYOUT_POST], 
        0, 1, &descriptorSets[R_DESC_SET_POST],
        0, NULL);

    vkCmdBeginRenderPass(*cmdBuf, rpassInfo, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdDraw(*cmdBuf, 3, 1, 0, 0);

    vkCmdEndRenderPass(*cmdBuf);
}

static void createShaderBindingTable(void)
{
    const VkPhysicalDeviceRayTracingPropertiesKHR rtprops = tanto_v_GetPhysicalDeviceRayTracingProperties();
    const uint32_t groupCount = 4;
    const uint32_t groupHandleSize = rtprops.shaderGroupHandleSize;
    const uint32_t baseAlignment = rtprops.shaderGroupBaseAlignment;
    const uint32_t sbtSize = groupCount * baseAlignment; // 3 shader groups: raygen, miss, closest hit

    uint8_t shaderHandleData[sbtSize];

    printf("ShaderGroup base alignment: %d\n", baseAlignment);
    printf("ShaderGroups total size   : %d\n", sbtSize);

    VkResult r;
    r = vkGetRayTracingShaderGroupHandlesKHR(device, pipelines[R_PIPE_RAYTRACE], 0, groupCount, sbtSize, shaderHandleData);
    assert( VK_SUCCESS == r );
    stbBlock = tanto_v_RequestBufferRegionAligned(sbtSize, baseAlignment, TANTO_V_MEMORY_HOST_GRAPHICS_TYPE);

    uint8_t* pSrc    = shaderHandleData;
    uint8_t* pTarget = stbBlock.hostData;

    for (int i = 0; i < groupCount; i++) 
    {
        memcpy(pTarget, pSrc + i * groupHandleSize, groupHandleSize);
        pTarget += baseAlignment;
    }

    printf("Created shader binding table\n");
}

void r_InitRenderer(void)
{
    InitPipelines();

    initOffscreenFrameBuffer();
    initPaintImage();

    createShaderBindingTable();
    initNonMeshDescriptors();

    brushDim.x = BRUSH_IMG_SIZE;
    brushDim.y = BRUSH_IMG_SIZE;

    //for (int i = 0; i < FRAME_COUNT; i++) 
    //{
    //    r_RequestFrame();
    //    
    //    r_UpdateRenderCommands();

    //    r_PresentFrame();
    //}
}

void r_UpdateRenderCommands(void)
{
    pushConstants.clearColor = (Vec4){0.1, 0.2, 0.5, 1.0};
    pushConstants.lightIntensity = 1.0;
    pushConstants.lightDir = (Vec3){-0.707106769, -0.5, -0.5};
    pushConstants.lightType = 0;
    pushConstants.colorOffset =  hapiMesh.colOffset / sizeof(Vec3);
    pushConstants.normalOffset = hapiMesh.norOffset / sizeof(Vec3);
    pushConstants.uvwOffset    = hapiMesh.uvwOffset / sizeof(Vec3);

    VkResult r;
    Tanto_R_Frame* frame = &frames[curFrameIndex];
    vkResetCommandPool(device, frame->commandPool, 0);
    VkCommandBufferBeginInfo cbbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    r = vkBeginCommandBuffer(frame->commandBuffer, &cbbi);

    VkClearValue clearValueColor = {0.002f, 0.023f, 0.009f, 1.0f};
    VkClearValue clearValueDepth = {1.0, 0};

    VkClearValue clears[] = {clearValueColor, clearValueDepth};

    const VkRenderPassBeginInfo rpassOffscreen = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .clearValueCount = 2,
        .pClearValues = clears,
        .renderArea = {{0, 0}, {TANTO_WINDOW_WIDTH, TANTO_WINDOW_HEIGHT}},
        .renderPass =  *offscreenFrameBuffer.pRenderPass,
        .framebuffer = offscreenFrameBuffer.handle,
    };

    const VkRenderPassBeginInfo rpassSwap = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .clearValueCount = 1,
        .pClearValues = clears,
        .renderArea = {{0, 0}, {TANTO_WINDOW_WIDTH, TANTO_WINDOW_HEIGHT}},
        .renderPass =  *frame->renderPass,
        .framebuffer = frame->frameBuffer 
    };

    //vkCmdBeginRenderPass(frame->commandBuffer, &rpassOffscreen, VK_SUBPASS_CONTENTS_INLINE);
    //vkCmdEndRenderPass(frame->commandBuffer);

    rayTrace(&frame->commandBuffer);
    rasterize(&frame->commandBuffer, &rpassOffscreen);
    postProc(&frame->commandBuffer, &rpassSwap);

    r = vkEndCommandBuffer(frame->commandBuffer);
    assert ( VK_SUCCESS == r );
}

Mat4* r_GetXform(r_XformType type)
{
    UboMatrices* matrices = (UboMatrices*)matrixBlock.hostData;
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
    assert (brushBlock.hostData);
    return (Brush*)brushBlock.hostData;
}

UboPlayer* r_GetPlayer(void)
{
    assert(playerBlock.hostData);
    return (UboPlayer*)playerBlock.hostData;
}

void r_LoadMesh(Tanto_R_Mesh mesh)
{
    hapiMesh = mesh;
    tanto_r_BuildBlas(&hapiMesh);
    tanto_r_BuildTlas();

    updateMeshDescriptors();
}

void r_ClearMesh(void)
{
    tanto_r_RayTraceDestroyAccelStructs();
    tanto_r_FreeMesh(hapiMesh);
}

void r_SavePaintImage(void)
{
    tanto_v_SaveImage(&paintImage, TANTO_V_IMAGE_FILE_PNG_TYPE);
}

void r_CleanUp(void)
{
    vkDestroyFramebuffer(device, offscreenFrameBuffer.handle, NULL);
    //vkDestroySampler(device, offscreenFrameBuffer.colorAttachment.sampler, NULL);
    //vkDestroyImageView(device, offscreenFrameBuffer.colorAttachment.view, NULL);
    //vkDestroyImage(device, offscreenFrameBuffer.colorAttachment.handle, NULL);
    //vkDestroyImageView(device, offscreenFrameBuffer.depthAttachment.view, NULL);
    //vkDestroyImage(device, offscreenFrameBuffer.depthAttachment.handle, NULL);
    tanto_v_DestroyImage(offscreenFrameBuffer.colorAttachment);
    tanto_v_DestroyImage(offscreenFrameBuffer.depthAttachment);
    //vkDestroySampler(device, paintImage.sampler, NULL);
    //vkDestroyImage(device, paintImage.handle, NULL);
    //vkDestroyImageView(device, paintImage.view, NULL);
}
