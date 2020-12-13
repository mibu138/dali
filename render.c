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
#include <tanto/v_command.h>
#include <tanto/r_renderpass.h>
#include <vulkan/vulkan_core.h>

#define SPVDIR "/home/michaelb/dev/painter/shaders/spv"

typedef Brush UboBrush;

static Tanto_V_BufferRegion  matrixRegion;
static Tanto_V_BufferRegion  brushRegion;
static Tanto_V_BufferRegion  stbPaintRegion;
static Tanto_V_BufferRegion  stbSelectRegion;
static Tanto_V_BufferRegion  playerRegion;
static Tanto_V_BufferRegion  selectionRegion;

static Tanto_R_Mesh          renderMesh;

static RtPushConstants pushConstants;

static VkPipeline      pipelineRaster;
static VkPipeline      pipelineRayTrace;
static VkPipeline      pipelinePost;
static VkPipeline      pipelineSelect;

static Tanto_V_Image        depthAttachment;
static Tanto_V_Image        paintImage;

static VkFramebuffer        swapchainFrameBuffers[TANTO_FRAME_COUNT];

static Vec2                 paintImageDim;
static Vec2                 brushDim;

static const VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
static const VkFormat paintFormat = VK_FORMAT_R8G8B8A8_UNORM;

static VkRenderPass swapchainRenderPass;

#define PAINT_IMG_SIZE 0x1000 // 0x1000 = 4096
#define BRUSH_IMG_SIZE 0x1000

typedef enum {
    R_PIPE_LAYOUT_RASTER,
    R_PIPE_LAYOUT_RAYTRACE,
    R_PIPE_LAYOUT_POST,
} R_PipelineLayoutId;

typedef enum {
    R_DESC_SET_RASTER,
    R_DESC_SET_RAYTRACE,
    R_DESC_SET_POST,
} R_DescriptorSetId;

static void initOffscreenAttachments(void)
{
    //initDepthAttachment();
    depthAttachment = tanto_v_CreateImage(
            TANTO_WINDOW_WIDTH, TANTO_WINDOW_HEIGHT,
            depthFormat,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_ASPECT_DEPTH_BIT, 
            VK_SAMPLE_COUNT_1_BIT);
}

static void initRenderPass(void)
{
    const VkAttachmentDescription attachmentColor = {
        .flags = 0,
        .format = tanto_r_GetSwapFormat(),
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
    };

    const VkAttachmentDescription attachmentDepth = {
        .flags = 0,
        .format = depthFormat,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
    };

    const VkAttachmentDescription attachments[] = {
        attachmentColor, attachmentDepth
    };

    VkAttachmentReference colorReference = {
        .attachment = 0,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };

    VkAttachmentReference depthReference = {
        .attachment = 1,
        .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
    };

    const VkSubpassDescription subpass = {
        .flags                   = 0,
        .pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .inputAttachmentCount    = 0,
        .pInputAttachments       = NULL,
        .colorAttachmentCount    = 1,
        .pColorAttachments       = &colorReference,
        .pResolveAttachments     = NULL,
        .pDepthStencilAttachment = &depthReference,
        .preserveAttachmentCount = 0,
        .pPreserveAttachments    = NULL,
    };

    Tanto_R_RenderPassInfo rpi = {
        .attachmentCount = 2,
        .pAttachments = attachments,
        .subpassCount = 1,
        .pSubpasses = &subpass,
    };

    tanto_r_CreateRenderPass(&rpi, &swapchainRenderPass);
}

static void initPaintImage(void)
{
    paintImageDim.x = PAINT_IMG_SIZE;
    paintImageDim.y = PAINT_IMG_SIZE;
    paintImage = tanto_v_CreateImageAndSampler(paintImageDim.x, paintImageDim.y, paintFormat, 
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | 
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 
            VK_IMAGE_ASPECT_COLOR_BIT,
            VK_FILTER_LINEAR, 
            1);

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
        .offset = renderMesh.vertexBlock.offset,
        .range  = renderMesh.vertexBlock.size,
        .buffer = renderMesh.vertexBlock.buffer,
    };

    VkDescriptorBufferInfo indexBufInfo = {
        .offset = renderMesh.indexBlock.offset,
        .range  = renderMesh.indexBlock.size,
        .buffer = renderMesh.indexBlock.buffer,
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
    matrixRegion = tanto_v_RequestBufferRegion(sizeof(UboMatrices), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            TANTO_V_MEMORY_HOST_GRAPHICS_TYPE);
    UboMatrices* matrices = (UboMatrices*)matrixRegion.hostData;
    matrices->model   = m_Ident_Mat4();
    matrices->view    = m_Ident_Mat4();
    matrices->proj    = m_Ident_Mat4();
    matrices->viewInv = m_Ident_Mat4();
    matrices->projInv = m_Ident_Mat4();

    brushRegion = tanto_v_RequestBufferRegion(sizeof(UboBrush), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            TANTO_V_MEMORY_HOST_GRAPHICS_TYPE);
    UboBrush* brush = (UboBrush*)brushRegion.hostData;
    memset(brush, 0, sizeof(Brush));
    brush->radius = 0.01;
    brush->mode = 1;

    playerRegion = tanto_v_RequestBufferRegion(sizeof(UboPlayer), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, TANTO_V_MEMORY_HOST_GRAPHICS_TYPE);
    memset(playerRegion.hostData, 0, sizeof(UboPlayer));

    selectionRegion = tanto_v_RequestBufferRegion(sizeof(Selection), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, TANTO_V_MEMORY_HOST_GRAPHICS_TYPE);


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
        .imageView   = paintImage.view,
        .sampler     = paintImage.sampler
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
            .dstSet = descriptorSets[R_DESC_SET_RAYTRACE],
            .dstBinding = 2,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &storageBufInfoSelection
        },{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstArrayElement = 0,
            .dstSet = descriptorSets[R_DESC_SET_POST],
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .pBufferInfo = &uniformInfoBrush,
    }};

    vkUpdateDescriptorSets(device, TANTO_ARRAY_SIZE(writes), writes, 0, NULL);
}

static void initDescSetsAndPipeLayouts(void)
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
            .id = R_DESC_SET_POST,
            .bindingCount = 1,
            .bindings = {{
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

    tanto_r_InitDescriptorSets(descSets, TANTO_ARRAY_SIZE(descSets));
    tanto_r_InitPipelineLayouts(pipelayouts, TANTO_ARRAY_SIZE(pipelayouts));
}

static void initPipelines(void)
{
    const Tanto_R_PipelineInfo pipeInfoRaster = {
        .type     = TANTO_R_PIPELINE_RASTER_TYPE,
        .layoutId = R_PIPE_LAYOUT_RASTER,
        .payload.rasterInfo = {
            .renderPass = swapchainRenderPass, 
            .vertexDescription = tanto_r_GetVertexDescription3D_4Vec3(),
            .frontFace = VK_FRONT_FACE_CLOCKWISE,
            .sampleCount = VK_SAMPLE_COUNT_1_BIT,
            .vertShader = SPVDIR"/raster-vert.spv", 
            .fragShader = SPVDIR"/raster-frag.spv"
        }};

    const Tanto_R_PipelineInfo pipeInfoRayTrace = {
        .type     = TANTO_R_PIPELINE_RAYTRACE_TYPE,
        .layoutId = R_PIPE_LAYOUT_RAYTRACE,
        .payload.rayTraceInfo = {
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
        }};

    const Tanto_R_PipelineInfo pipeInfoPost = {
        .type     = TANTO_R_PIPELINE_RASTER_TYPE,
        .layoutId = R_PIPE_LAYOUT_POST,
        .payload.rasterInfo = {
            .renderPass = swapchainRenderPass,
            .sampleCount = VK_SAMPLE_COUNT_1_BIT,
            .frontFace   = VK_FRONT_FACE_CLOCKWISE,
            .blendMode   = TANTO_R_BLEND_MODE_OVER,
            .vertShader = tanto_r_FullscreenTriVertShader(),
            .fragShader = SPVDIR"/post-frag.spv"
        }};

    const Tanto_R_PipelineInfo pipeInfoSelect = {
        .type     = TANTO_R_PIPELINE_RAYTRACE_TYPE,
        .layoutId = R_PIPE_LAYOUT_RAYTRACE,
        .payload.rayTraceInfo = {
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

    tanto_r_CreatePipeline(&pipeInfoRaster, &pipelineRaster);
    tanto_r_CreatePipeline(&pipeInfoRayTrace, &pipelineRayTrace);
    tanto_r_CreatePipeline(&pipeInfoPost, &pipelinePost);
    tanto_r_CreatePipeline(&pipeInfoSelect, &pipelineSelect);
}

static void rayTraceSelect(const VkCommandBuffer* cmdBuf)
{
    vkCmdBindPipeline(*cmdBuf, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipelineSelect); 

    vkCmdBindDescriptorSets(*cmdBuf, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, 
            pipelineLayouts[R_PIPE_LAYOUT_RAYTRACE], 0, 3, descriptorSets, 0, NULL);

    vkCmdPushConstants(*cmdBuf, pipelineLayouts[R_PIPE_LAYOUT_RAYTRACE], 
        VK_SHADER_STAGE_RAYGEN_BIT_KHR | 
        VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
        VK_SHADER_STAGE_MISS_BIT_KHR, 0, sizeof(RtPushConstants), &pushConstants);

    const VkPhysicalDeviceRayTracingPropertiesKHR rtprops = tanto_v_GetPhysicalDeviceRayTracingProperties();
    const VkDeviceSize progSize = rtprops.shaderGroupBaseAlignment;
    const VkDeviceSize sbtSize = rtprops.shaderGroupBaseAlignment * 3;
    const VkDeviceSize baseAlignment = stbSelectRegion.offset;
    const VkDeviceSize rayGenOffset   = baseAlignment;
    const VkDeviceSize missOffset     = baseAlignment + 1u * progSize;
    const VkDeviceSize hitGroupOffset = baseAlignment + 2u * progSize; // have to jump over 1 miss shaders

    assert( rayGenOffset % rtprops.shaderGroupBaseAlignment == 0 );

    const VkStridedBufferRegionKHR raygenShaderBindingTable = {
        .buffer = stbSelectRegion.buffer,
        .offset = rayGenOffset,
        .stride = progSize,
        .size   = sbtSize,
    };

    const VkStridedBufferRegionKHR missShaderBindingTable = {
        .buffer = stbSelectRegion.buffer,
        .offset = missOffset,
        .stride = progSize,
        .size   = sbtSize,
    };

    const VkStridedBufferRegionKHR hitShaderBindingTable = {
        .buffer = stbSelectRegion.buffer,
        .offset = hitGroupOffset,
        .stride = progSize,
        .size   = sbtSize,
    };

    const VkStridedBufferRegionKHR callableShaderBindingTable = {
    };

    vkCmdTraceRaysKHR(*cmdBuf, &raygenShaderBindingTable,
            &missShaderBindingTable, &hitShaderBindingTable,
            &callableShaderBindingTable, 1, 
            1, 1);
}

static void rayTrace(const VkCommandBuffer* cmdBuf)
{
    vkCmdBindPipeline(*cmdBuf, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipelineRayTrace);

    vkCmdBindDescriptorSets(*cmdBuf, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, 
            pipelineLayouts[R_PIPE_LAYOUT_RAYTRACE], 0, 3, descriptorSets, 0, NULL);

    vkCmdPushConstants(*cmdBuf, pipelineLayouts[R_PIPE_LAYOUT_RAYTRACE], 
        VK_SHADER_STAGE_RAYGEN_BIT_KHR | 
        VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
        VK_SHADER_STAGE_MISS_BIT_KHR, 0, sizeof(RtPushConstants), &pushConstants);

    const VkPhysicalDeviceRayTracingPropertiesKHR rtprops = tanto_v_GetPhysicalDeviceRayTracingProperties();
    const VkDeviceSize progSize = rtprops.shaderGroupBaseAlignment;
    const VkDeviceSize sbtSize = rtprops.shaderGroupBaseAlignment * 3;
    const VkDeviceSize baseAlignment = stbPaintRegion.offset;
    const VkDeviceSize rayGenOffset   = baseAlignment;
    const VkDeviceSize missOffset     = baseAlignment + 1u * progSize;
    const VkDeviceSize hitGroupOffset = baseAlignment + 2u * progSize; // have to jump over 1 miss shaders

    printf("Prog\n");

    assert( rayGenOffset % rtprops.shaderGroupBaseAlignment == 0 );

    const VkStridedBufferRegionKHR raygenShaderBindingTable = {
        .buffer = stbPaintRegion.buffer,
        .offset = rayGenOffset,
        .stride = progSize,
        .size   = sbtSize,
    };

    const VkStridedBufferRegionKHR missShaderBindingTable = {
        .buffer = stbPaintRegion.buffer,
        .offset = missOffset,
        .stride = progSize,
        .size   = sbtSize,
    };

    const VkStridedBufferRegionKHR hitShaderBindingTable = {
        .buffer = stbPaintRegion.buffer,
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
    vkCmdBindPipeline(*cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineRaster);

    vkCmdBindDescriptorSets(
        *cmdBuf, 
        VK_PIPELINE_BIND_POINT_GRAPHICS, 
        pipelineLayouts[R_PIPE_LAYOUT_RASTER], 
        0, 1, &descriptorSets[R_DESC_SET_RASTER], 
        0, NULL);

    vkCmdBeginRenderPass(*cmdBuf, rpassInfo, VK_SUBPASS_CONTENTS_INLINE);

        VkBuffer vertBuffers[4] = {
            renderMesh.vertexBlock.buffer,
            renderMesh.vertexBlock.buffer,
            renderMesh.vertexBlock.buffer,
            renderMesh.vertexBlock.buffer
        };

        const VkDeviceSize vertOffsets[4] = {
            renderMesh.posOffset + renderMesh.vertexBlock.offset, 
            renderMesh.colOffset + renderMesh.vertexBlock.offset,
            renderMesh.norOffset + renderMesh.vertexBlock.offset,
            renderMesh.uvwOffset + renderMesh.vertexBlock.offset
        };

        vkCmdBindVertexBuffers(
            *cmdBuf, 0, TANTO_ARRAY_SIZE(vertOffsets), 
            vertBuffers, vertOffsets);

        vkCmdBindIndexBuffer(
            *cmdBuf, 
            renderMesh.indexBlock.buffer, 
            renderMesh.indexBlock.offset, TANTO_VERT_INDEX_TYPE);

        vkCmdDrawIndexed(*cmdBuf, 
            renderMesh.indexCount, 1, 0, 
            0, 0);

        vkCmdBindPipeline(*cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelinePost);

        vkCmdBindDescriptorSets(
            *cmdBuf, 
            VK_PIPELINE_BIND_POINT_GRAPHICS, 
            pipelineLayouts[R_PIPE_LAYOUT_POST], 
            0, 1, &descriptorSets[R_DESC_SET_POST],
            0, NULL);

        vkCmdDraw(*cmdBuf, 3, 1, 0, 0);

    vkCmdEndRenderPass(*cmdBuf);
}

static void createShaderBindingTableSelect(void)
{
    const VkPhysicalDeviceRayTracingPropertiesKHR rtprops = tanto_v_GetPhysicalDeviceRayTracingProperties();
    const uint32_t groupCount = 3;
    const uint32_t groupHandleSize = rtprops.shaderGroupHandleSize;
    const uint32_t baseAlignment = rtprops.shaderGroupBaseAlignment;
    const uint32_t sbtSize = groupCount * baseAlignment; // 3 shader groups: raygen, miss, closest hit

    uint8_t shaderHandleData[sbtSize];

    printf("ShaderGroup base alignment: %d\n", baseAlignment);
    printf("ShaderGroups total size   : %d\n", sbtSize);

    VkResult r;
    r = vkGetRayTracingShaderGroupHandlesKHR(device, pipelineSelect, 0, groupCount, sbtSize, shaderHandleData);
    assert( VK_SUCCESS == r );
    stbSelectRegion = tanto_v_RequestBufferRegionAligned(sbtSize, baseAlignment, TANTO_V_MEMORY_HOST_GRAPHICS_TYPE);

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
    const VkPhysicalDeviceRayTracingPropertiesKHR rtprops = tanto_v_GetPhysicalDeviceRayTracingProperties();
    const uint32_t groupCount = 3;
    const uint32_t groupHandleSize = rtprops.shaderGroupHandleSize;
    const uint32_t baseAlignment = rtprops.shaderGroupBaseAlignment;
    const uint32_t sbtSize = groupCount * baseAlignment; // 3 shader groups: raygen, miss, closest hit

    uint8_t shaderHandleData[sbtSize];

    printf("ShaderGroup base alignment: %d\n", baseAlignment);
    printf("ShaderGroups total size   : %d\n", sbtSize);

    V_ASSERT( vkGetRayTracingShaderGroupHandlesKHR(device, pipelineRayTrace, 0, groupCount, sbtSize, shaderHandleData) );
    stbPaintRegion = tanto_v_RequestBufferRegionAligned(sbtSize, baseAlignment, TANTO_V_MEMORY_HOST_GRAPHICS_TYPE);

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
    pushConstants.clearColor = (Vec4){0.1, 0.2, 0.5, 1.0};
    pushConstants.lightIntensity = 1.0;
    pushConstants.lightDir = (Vec3){-0.707106769, -0.5, -0.5};
    pushConstants.lightType = 0;
    pushConstants.posOffset =    renderMesh.posOffset / sizeof(Vec3);
    pushConstants.colorOffset =  renderMesh.colOffset / sizeof(Vec3);
    pushConstants.normalOffset = renderMesh.norOffset / sizeof(Vec3);
    pushConstants.uvwOffset    = renderMesh.uvwOffset / sizeof(Vec3);
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
}

static void cleanUpSwapchainDependent(void)
{
    for (int i = 0; i < TANTO_FRAME_COUNT; i++) 
    {
        vkDestroyFramebuffer(device, swapchainFrameBuffers[i], NULL);
    }
    vkDestroyPipeline(device, pipelineRaster, NULL);
    vkDestroyPipeline(device, pipelineRayTrace, NULL);
    vkDestroyPipeline(device, pipelineSelect, NULL);
    vkDestroyPipeline(device, pipelinePost, NULL);
    tanto_v_FreeImage(&depthAttachment);
}

void r_InitRenderer(void)
{
    initRenderPass();
    initDescSetsAndPipeLayouts();
    initPipelines();

    initOffscreenAttachments();
    initPaintImage();

    createShaderBindingTablePaint();
    createShaderBindingTableSelect();
    initNonMeshDescriptors();

    brushDim.x = BRUSH_IMG_SIZE;
    brushDim.y = BRUSH_IMG_SIZE;

    initFramebuffers();
}

int r_GetSelectionPos(Vec3* v)
{
    Tanto_V_CommandPool pool = tanto_v_RequestOneTimeUseCommand();

    rayTraceSelect(&pool.buffer);

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

void r_UpdateRenderCommands(const int8_t frameIndex)
{
    updatePushConstants();

    Tanto_R_Frame* frame = tanto_r_GetFrame(frameIndex);
    vkResetCommandPool(device, frame->commandPool, 0);
    VkCommandBufferBeginInfo cbbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    V_ASSERT( vkBeginCommandBuffer(frame->commandBuffer, &cbbi) );

    VkClearValue clearValueColor = {0.002f, 0.003f, 0.009f, 1.0f};
    VkClearValue clearValueDepth = {1.0, 0};

    VkClearValue clears[] = {clearValueColor, clearValueDepth};

    const VkRenderPassBeginInfo rpassSwap = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .clearValueCount = 2,
        .pClearValues = clears,
        .renderArea = {{0, 0}, {TANTO_WINDOW_WIDTH, TANTO_WINDOW_HEIGHT}},
        .renderPass =  swapchainRenderPass,
        .framebuffer = swapchainFrameBuffers[frameIndex]
    };

    rayTrace(&frame->commandBuffer);
    rasterize(&frame->commandBuffer, &rpassSwap);

    V_ASSERT( vkEndCommandBuffer(frame->commandBuffer) );
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

void r_LoadMesh(Tanto_R_Mesh mesh)
{
    renderMesh = mesh;
    tanto_r_BuildBlas(&renderMesh);
    tanto_r_BuildTlas();

    updateMeshDescriptors();
}

void r_ClearMesh(void)
{
    tanto_r_RayTraceDestroyAccelStructs();
    tanto_r_FreeMesh(renderMesh);
}

void r_RecreateSwapchain(void)
{
    vkDeviceWaitIdle(device);
    cleanUpSwapchainDependent();

    tanto_r_RecreateSwapchain();
    initOffscreenAttachments();
    initPipelines();
    initFramebuffers();

    for (int i = 0; i < TANTO_FRAME_COUNT; i++) 
    {
        r_UpdateRenderCommands(i);
    }
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
    if (strbuf[len] == '\n')  strbuf[len--] = '\0'; 
    const char* ext = strbuf + len - 4;
    TANTO_DEBUG_PRINT("%s", ext);
    Tanto_V_ImageFileType fileType;
    if (strncmp(ext, "png", 3) == 0) fileType = TANTO_V_IMAGE_FILE_TYPE_PNG;
    else if (strncmp(ext, "jpg", 3) == 0) fileType = TANTO_V_IMAGE_FILE_TYPE_JPG;
    else 
    {
        printf("Bad extension.\n");
        return;
    }
    tanto_v_SaveImage(&paintImage, fileType, strbuf);
}

void r_ClearPaintImage(void)
{
    tanto_v_ClearColorImage(&paintImage);
}

void r_CleanUp(void)
{
    cleanUpSwapchainDependent();
    tanto_v_FreeImage(&paintImage);
    vkDestroyRenderPass(device, swapchainRenderPass, NULL);
}

const Tanto_R_Mesh* r_GetMesh(void)
{
    return &renderMesh;
}

