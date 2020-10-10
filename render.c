#include "render.h"
#include "common.h"
#include <memory.h>
#include <assert.h>
#include <string.h>
#include <tanto/r_render.h>
#include <tanto/v_video.h>
#include <tanto/t_def.h>
#include <tanto/t_utils.h>
#include <tanto/r_pipeline.h>
#include <tanto/r_raytrace.h>

typedef struct {
    Mat4 model;
    Mat4 view;
    Mat4 proj;
    Mat4 viewInv;
    Mat4 projInv;
} UboMatrices;

static Tanto_V_Block*      matrixBlock;
static UboMatrices*  matrices;
static const Tanto_R_Mesh*   hapiMesh;
static Tanto_V_Block*      stbBlock;

static RtPushConstants pushConstants;

static Tanto_R_FrameBuffer  offscreenFrameBuffer;
static bool createdPipelines;

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

static void initDepthAttachment(void)
{
    VkImageCreateInfo imageInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = depthFormat,
        .extent = {TANTO_WINDOW_WIDTH, TANTO_WINDOW_HEIGHT, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 1,
        .pQueueFamilyIndices = &graphicsQueueFamilyIndex,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };

    VkResult r;

    r = vkCreateImage(device, &imageInfo, NULL, &offscreenFrameBuffer.depthAttachment.handle);
    assert( VK_SUCCESS == r );

    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(device, offscreenFrameBuffer.depthAttachment.handle, &memReqs);

#ifndef NDEBUG
    V1_PRINT("Depth image reqs: \nSize: %ld\nAlignment: %ld\nTypes: ", 
            memReqs.size, memReqs.alignment);
    bitprint(&memReqs.memoryTypeBits, 32);
#endif

    tanto_v_BindImageToMemory(offscreenFrameBuffer.depthAttachment.handle, memReqs.size);
    
    VkImageViewCreateInfo viewInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = offscreenFrameBuffer.depthAttachment.handle,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .components = {0, 0, 0, 0}, // no swizzling
        .format = depthFormat,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1
        }
    };

    r = vkCreateImageView(device, &viewInfo, NULL, &offscreenFrameBuffer.depthAttachment.view);
    assert( VK_SUCCESS == r );
}

static void initOffscreenFrameBuffer(void)
{
    initDepthAttachment();
    VkImageCreateInfo imageInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = offscreenColorFormat,
        .extent = {TANTO_WINDOW_WIDTH, TANTO_WINDOW_HEIGHT, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = 
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | 
            VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_STORAGE_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 1,
        .pQueueFamilyIndices = &graphicsQueueFamilyIndex,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };

    VkResult r;

    r = vkCreateImage(device, &imageInfo, NULL, &offscreenFrameBuffer.colorAttachment.handle);
    assert( VK_SUCCESS == r );

    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(device, offscreenFrameBuffer.colorAttachment.handle, &memReqs);

#ifndef NDEBUG
    V1_PRINT("Offscreen framebuffer reqs: \nSize: %ld\nAlignment: %ld\nTypes: ", 
            memReqs.size, memReqs.alignment);
    bitprint(&memReqs.memoryTypeBits, 32);
#endif

    tanto_v_BindImageToMemory(offscreenFrameBuffer.colorAttachment.handle, memReqs.size);

    VkImageViewCreateInfo viewInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = offscreenFrameBuffer.colorAttachment.handle,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .components = {0, 0, 0, 0}, // no swizzling
        .format = offscreenColorFormat,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1
        }
    };

    r = vkCreateImageView(device, &viewInfo, NULL, &offscreenFrameBuffer.colorAttachment.view);
    assert( VK_SUCCESS == r );


    VkSamplerCreateInfo samplerInfo = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .mipLodBias = 0.0,
        .anisotropyEnable = VK_FALSE,
        .compareEnable = VK_FALSE,
        .minLod = 0.0,
        .maxLod = 0.0, // must both be 0 when using unnormalizedCoordinates
        .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
        .unnormalizedCoordinates = VK_FALSE // allow us to window coordinates in frag shader
    };

    r = vkCreateSampler(device, &samplerInfo, NULL, &offscreenFrameBuffer.colorAttachment.sampler);
    assert( VK_SUCCESS == r );

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

    r = vkCreateFramebuffer(device, &framebufferInfo, NULL, &offscreenFrameBuffer.handle);
    assert( VK_SUCCESS == r );

    {
        VkCommandPool cmdPool;

        VkCommandPoolCreateInfo poolInfo = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .queueFamilyIndex = graphicsQueueFamilyIndex,
        };

        vkCreateCommandPool(device, &poolInfo, NULL, &cmdPool);

        VkCommandBufferAllocateInfo cmdBufInfo = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandBufferCount = 1,
            .commandPool = cmdPool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        };

        VkCommandBuffer cmdBuf;

        vkAllocateCommandBuffers(device, &cmdBufInfo, &cmdBuf);

        VkCommandBufferBeginInfo beginInfo = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };

        vkBeginCommandBuffer(cmdBuf, &beginInfo);

        VkImageSubresourceRange subResRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseArrayLayer = 0,
            .baseMipLevel = 0,
            .levelCount = 1,
            .layerCount = 1,
        };

        VkImageMemoryBarrier imgBarrier = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .image = offscreenFrameBuffer.colorAttachment.handle,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .subresourceRange = subResRange,
            .srcAccessMask = 0,
            .dstAccessMask = 0,
        };

        vkCmdPipelineBarrier(cmdBuf, 
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 
                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &imgBarrier);

        vkEndCommandBuffer(cmdBuf);

        tanto_v_SubmitToQueueWait(&cmdBuf, TANTO_V_QUEUE_GRAPHICS_TYPE, 0);

        vkDestroyCommandPool(device, cmdPool, NULL);
    }
}

static void initDescriptors(void)
{
    matrixBlock = tanto_v_RequestBlock(sizeof(UboMatrices), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    matrices = (UboMatrices*)matrixBlock->address;
    matrices->model   = m_Ident_Mat4();
    matrices->view    = m_Ident_Mat4();
    matrices->proj    = m_Ident_Mat4();
    matrices->viewInv = m_Ident_Mat4();
    matrices->projInv = m_Ident_Mat4();

    VkDescriptorBufferInfo uniformInfo = {
        .range  =  matrixBlock->size,
        .offset =  matrixBlock->vOffset,
        .buffer = *matrixBlock->vBuffer,
    };

#if RAY_TRACE
    VkWriteDescriptorSetAccelerationStructureKHR asInfo = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
        .accelerationStructureCount = 1,
        .pAccelerationStructures    = &topLevelAS
    };

    VkDescriptorImageInfo imageInfo = {
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .imageView   = offscreenFrameBuffer.colorAttachment.view,
        .sampler     = offscreenFrameBuffer.colorAttachment.sampler
    };
#endif

    VkDescriptorBufferInfo vertBufInfo = {
        .offset = hapiMesh->vertexBlock->vOffset,
        .range  = hapiMesh->vertexBlock->size,
        .buffer = *hapiMesh->vertexBlock->vBuffer,
    };

    VkDescriptorBufferInfo indexBufInfo = {
        .offset =  hapiMesh->indexBlock->vOffset,
        .range  =  hapiMesh->indexBlock->size,
        .buffer = *hapiMesh->indexBlock->vBuffer,
    };

    VkWriteDescriptorSet writes[] = {{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstArrayElement = 0,
            .dstSet = descriptorSets[R_DESC_SET_RASTER],
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .pBufferInfo = &uniformInfo
        },{
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
#if RAY_TRACE
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstArrayElement = 0,
            .dstSet = descriptorSets[R_DESC_SET_RAYTRACE],
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
            .pNext = &asInfo
        },{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstArrayElement = 0,
            .dstSet = descriptorSets[R_DESC_SET_RAYTRACE],
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .pImageInfo = &imageInfo
        },{
#endif
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstArrayElement = 0,
            .dstSet = descriptorSets[R_DESC_SET_POST],
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &imageInfo
    }};

    vkUpdateDescriptorSets(device, TANTO_ARRAY_SIZE(writes), writes, 0, NULL);
}

static void rayTrace(const VkCommandBuffer* cmdBuf)
{
    vkCmdBindPipeline(*cmdBuf, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipelines[R_PIPE_RAYTRACE]);

    vkCmdBindDescriptorSets(*cmdBuf, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, 
            pipelineLayouts[R_PIPE_LAYOUT_RAYTRACE], 0, 2, descriptorSets, 0, NULL);

    vkCmdPushConstants(*cmdBuf, pipelineLayouts[R_PIPE_LAYOUT_RAYTRACE], 
        VK_SHADER_STAGE_RAYGEN_BIT_KHR | 
        VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
        VK_SHADER_STAGE_MISS_BIT_KHR, 0, sizeof(RtPushConstants), &pushConstants);

    const VkPhysicalDeviceRayTracingPropertiesKHR rtprops = tanto_v_GetPhysicalDeviceRayTracingProperties();
    const VkDeviceSize progSize = rtprops.shaderGroupBaseAlignment;
    const VkDeviceSize sbtSize = rtprops.shaderGroupBaseAlignment * 4;
    const VkDeviceSize baseAlignment = stbBlock->vOffset;
    const VkDeviceSize rayGenOffset   = baseAlignment;
    const VkDeviceSize missOffset     = baseAlignment + 1u * progSize;
    const VkDeviceSize hitGroupOffset = baseAlignment + 3u * progSize; // have to jump over 2 miss shaders

    printf("Prog\n");

    assert( rayGenOffset % rtprops.shaderGroupBaseAlignment == 0 );

    const VkStridedBufferRegionKHR raygenShaderBindingTable = {
        .buffer = *stbBlock->vBuffer,
        .offset = rayGenOffset,
        .stride = progSize,
        .size   = sbtSize,
    };

    const VkStridedBufferRegionKHR missShaderBindingTable = {
        .buffer = *stbBlock->vBuffer,
        .offset = missOffset,
        .stride = progSize,
        .size   = sbtSize,
    };

    const VkStridedBufferRegionKHR hitShaderBindingTable = {
        .buffer = *stbBlock->vBuffer,
        .offset = hitGroupOffset,
        .stride = progSize,
        .size   = sbtSize,
    };

    const VkStridedBufferRegionKHR callableShaderBindingTable = {
    };

    vkCmdTraceRaysKHR(*cmdBuf, &raygenShaderBindingTable, &missShaderBindingTable, &hitShaderBindingTable, &callableShaderBindingTable, TANTO_WINDOW_WIDTH, TANTO_WINDOW_WIDTH, 1);

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

    if (hapiMesh)
    {
        VkBuffer vertBuffers[3] = {
            *hapiMesh->vertexBlock->vBuffer,
            *hapiMesh->vertexBlock->vBuffer,
            *hapiMesh->vertexBlock->vBuffer
        };

        const VkDeviceSize vertOffsets[3] = {
            hapiMesh->posOffset, 
            hapiMesh->colOffset,
            hapiMesh->norOffset
        };

        vkCmdBindVertexBuffers(
            *cmdBuf, 0, TANTO_ARRAY_SIZE(vertOffsets), 
            vertBuffers, vertOffsets);

        vkCmdBindIndexBuffer(
            *cmdBuf, 
            *hapiMesh->indexBlock->vBuffer, 
            hapiMesh->indexBlock->vOffset, TANTO_VERT_INDEX_TYPE);

        vkCmdDrawIndexed(*cmdBuf, 
            hapiMesh->indexCount, 1, 0, 
            hapiMesh->vertexBlock->vOffset, 0);
    }

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
    stbBlock = tanto_v_RequestBlockAligned(sbtSize, baseAlignment);

    uint8_t* pSrc    = shaderHandleData;
    uint8_t* pTarget = stbBlock->address;

    for (int i = 0; i < groupCount; i++) 
    {
        memcpy(pTarget, pSrc + i * groupHandleSize, groupHandleSize);
        pTarget += baseAlignment;
    }

    printf("Created shader binding table\n");
}

static void InitPipelines(void)
{
    const Tanto_R_DescriptorSet descSets[] = {{
            R_DESC_SET_RASTER,
            3, // binding count
            {   {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR},
                {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR},
                {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR}}
        },{
            R_DESC_SET_RAYTRACE,
            2,
            {   {1, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 
                    VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR},
                {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 
                    VK_SHADER_STAGE_RAYGEN_BIT_KHR}}
        },{
            R_DESC_SET_POST,
            1,
            {
                {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT}}
    }};

    const VkPushConstantRange pushConstantRt = {
        .stageFlags = 
            VK_SHADER_STAGE_RAYGEN_BIT_KHR |
            VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
            VK_SHADER_STAGE_MISS_BIT_KHR,
        .offset = 0,
        .size = sizeof(RtPushConstants)
    };

    const Tanto_R_PipelineLayout pipelayouts[] = {{
        R_PIPE_LAYOUT_RASTER, 1, {R_DESC_SET_RASTER}
    },{
        R_PIPE_LAYOUT_RAYTRACE, 2, {R_DESC_SET_RASTER, R_DESC_SET_RAYTRACE}, 1, {pushConstantRt}
    },{
        R_PIPE_LAYOUT_POST, 1, {R_DESC_SET_POST}
    }};

    const Tanto_R_PipelineInfo pipeInfos[] = {{
        R_PIPE_RASTER,
        TANTO_R_PIPELINE_RASTER_TYPE,
        R_PIPE_LAYOUT_RASTER,
        {TANTO_R_RENDER_PASS_OFFSCREEN_TYPE, TANTO_SPVDIR"/default-vert.spv", TANTO_SPVDIR"/default-frag.spv"},
        {}
    },{
        R_PIPE_RAYTRACE,
        TANTO_R_PIPELINE_RAYTRACE_TYPE,
        R_PIPE_LAYOUT_RAYTRACE
    },{
        R_PIPE_POST,
        TANTO_R_PIPELINE_POSTPROC_TYPE,
        R_PIPE_LAYOUT_POST,
        {TANTO_R_RENDER_PASS_SWAPCHAIN_TYPE, "", TANTO_SPVDIR"/post-frag.spv"}
    }};

    tanto_r_InitDescriptorSets(descSets, TANTO_ARRAY_SIZE(descSets));
    tanto_r_InitPipelineLayouts(pipelayouts, TANTO_ARRAY_SIZE(pipelayouts));
    tanto_r_InitPipelines(pipeInfos, TANTO_ARRAY_SIZE(pipeInfos));
}

void r_InitRenderCommands(void)
{
    if (!createdPipelines)
    {
        InitPipelines();
        createdPipelines = true;
    }
    assert(hapiMesh);

    createShaderBindingTable();

    initOffscreenFrameBuffer();
    initDescriptors();

    pushConstants.clearColor = (Vec4){0.1, 0.2, 0.5, 1.0};
    pushConstants.lightIntensity = 1.0;
    pushConstants.lightDir = (Vec3){-0.707106769, -0.5, -0.5};
    pushConstants.lightType = 0;
    pushConstants.colorOffset =  hapiMesh->colOffset / sizeof(Vec3);
    pushConstants.normalOffset = hapiMesh->norOffset / sizeof(Vec3);

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

    if (parms.mode == MODE_RAY)
        rayTrace(&frame->commandBuffer);
    else 
        rasterize(&frame->commandBuffer, &rpassOffscreen);

    postProc(&frame->commandBuffer, &rpassSwap);

    r = vkEndCommandBuffer(frame->commandBuffer);
    assert ( VK_SUCCESS == r );
}

Mat4* r_GetXform(r_XformType type)
{
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

void r_LoadMesh(const Tanto_R_Mesh* mesh)
{
    hapiMesh = mesh;
}

void r_CommandCleanUp(void)
{
    vkDestroySampler(device, offscreenFrameBuffer.colorAttachment.sampler, NULL);
    vkDestroyFramebuffer(device, offscreenFrameBuffer.handle, NULL);
    vkDestroyImageView(device, offscreenFrameBuffer.colorAttachment.view, NULL);
    vkDestroyImage(device, offscreenFrameBuffer.colorAttachment.handle, NULL);
    vkDestroyImageView(device, offscreenFrameBuffer.depthAttachment.view, NULL);
    vkDestroyImage(device, offscreenFrameBuffer.depthAttachment.handle, NULL);
}
