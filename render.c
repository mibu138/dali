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
#include <tanto/v_video.h>
#include <tanto/r_renderpass.h>
#include <vulkan/vulkan_core.h>
#include "undo.h"

#define SPVDIR "/home/michaelb/dev/painter/shaders/spv"

typedef Brush UboBrush;
typedef Tanto_V_Command Command;
typedef Tanto_V_Image   Image;

typedef struct {
    Vec4 clearColor;
    Vec3 lightDir;
    float lightIntensity;
    int   lightType;
    uint32_t posOffset;
    uint32_t normalOffset;
    uint32_t uvwOffset;
} RtPushConstants;

typedef struct {
    uint32_t index;
} RasterPushConstants;

enum {
    LAYOUT_RASTER,
    LAYOUT_RAYTRACE,
    LAYOUT_POST,
    LAYOUT_COMP
};

enum {
    DESC_SET_RASTER,
    DESC_SET_RAYTRACE,
    DESC_SET_POST,
    DESC_SET_APPLY_PAINT,
    DESC_SET_COMP
};

enum {
    PIPELINE_RASTER,
    PIPELINE_POST,
    PIPELINE_COMP_1,
    PIPELINE_COMP_2,
    PIPELINE_COMP_SINGLE
};

enum {
    PIPELINE_RAY_TRACE,
    PIPELINE_SELECT,
};

typedef Tanto_V_BufferRegion BufferRegion;

static BufferRegion  matrixRegion;
static BufferRegion  brushRegion;
static BufferRegion  playerRegion;
static BufferRegion  selectionRegion;

static Tanto_R_Primitive     renderPrim;

static RtPushConstants     rtPushConstants;

static VkPipelineLayout pipelineLayouts[TANTO_MAX_PIPELINES];
static VkPipeline       graphicsPipelines[TANTO_MAX_PIPELINES];
static VkPipeline       raytracePipelines[TANTO_MAX_PIPELINES];

static Tanto_R_ShaderBindingTable sbtPaint;
static Tanto_R_ShaderBindingTable sbtSelect;

static VkDescriptorSetLayout descriptorSetLayouts[TANTO_MAX_DESCRIPTOR_SETS];
static Tanto_R_Description   description;

static Tanto_V_Image   depthAttachment;

static uint32_t graphicsQueueFamilyIndex;
static uint32_t transferQueueFamilyIndex;

#define TEXTURE_SIZE 0x1000 // 0x1000 = 4096

static Command releaseImageCommand;
static Command transferImageCommand;
static Command acquireImageCommand;

static Command renderCommands[TANTO_FRAME_COUNT];

static Image   imageA; // will use for brush and then as final frambuffer target
static Image   imageB;
static Image   imageC; // primarily background layers
static Image   imageD; // primarily foreground layers

static VkFramebuffer   compositeFrameBuffer;
static VkFramebuffer   backgroundFrameBuffer;
static VkFramebuffer   foregroundFrameBuffer;
static VkFramebuffer   swapchainFrameBuffers[TANTO_FRAME_COUNT];

static const VkFormat depthFormat   = VK_FORMAT_D32_SFLOAT;
static const VkFormat textureFormat = VK_FORMAT_R8G8B8A8_UNORM;

static VkRenderPass singleCompositeRenderPass;
static VkRenderPass compositeRenderPass;
static VkRenderPass swapchainRenderPass;

static Tanto_R_AccelerationStructure bottomLevelAS;
static Tanto_R_AccelerationStructure topLevelAS;

static L_LayerId curLayerId;

static bool needsToBackupLayer;
static bool needsToUndo;

static void updateRenderCommands(const int8_t frameIndex);
static void initOffscreenAttachments(void);
static void updatePrimDescriptors(void);
static void initNonMeshDescriptors(void);
static void initDescSetsAndPipeLayouts(void);
static void initPipelines(void);
static void rayTraceSelect(const VkCommandBuffer cmdBuf);
static void paint(const VkCommandBuffer cmdBuf);
static void rasterize(const VkCommandBuffer cmdBuf);
static void updatePushConstants(void);
static void cleanUpSwapchainDependent(void);
static void updateRenderCommands(const int8_t frameIndex);
static void onRecreateSwapchain(void);

static void initOffscreenAttachments(void)
{
    depthAttachment = tanto_v_CreateImage(
            TANTO_WINDOW_WIDTH, TANTO_WINDOW_HEIGHT,
            depthFormat,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_ASPECT_DEPTH_BIT, 
            VK_SAMPLE_COUNT_1_BIT,
            1,
            graphicsQueueFamilyIndex);
}

static void initPaintImages(void)
{
    imageA = tanto_v_CreateImageAndSampler(TEXTURE_SIZE, TEXTURE_SIZE, textureFormat, 
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | 
            VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT,
            VK_SAMPLE_COUNT_1_BIT,
            1,
            VK_FILTER_LINEAR, 
            graphicsQueueFamilyIndex);

    imageB = tanto_v_CreateImageAndSampler(TEXTURE_SIZE, TEXTURE_SIZE, textureFormat, 
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 
            VK_IMAGE_ASPECT_COLOR_BIT,
            VK_SAMPLE_COUNT_1_BIT,
            1,
            VK_FILTER_LINEAR, 
            graphicsQueueFamilyIndex);

    imageC = tanto_v_CreateImageAndSampler(TEXTURE_SIZE, TEXTURE_SIZE, textureFormat, 
            VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | 
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 
            VK_IMAGE_ASPECT_COLOR_BIT,
            VK_SAMPLE_COUNT_1_BIT,
            1,
            VK_FILTER_LINEAR, 
            graphicsQueueFamilyIndex);
    
    imageD = tanto_v_CreateImageAndSampler(TEXTURE_SIZE, TEXTURE_SIZE, textureFormat, 
            VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | 
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 
            VK_IMAGE_ASPECT_COLOR_BIT,
            VK_SAMPLE_COUNT_1_BIT,
            1,
            VK_FILTER_LINEAR, 
            graphicsQueueFamilyIndex);

    tanto_v_TransitionImageLayout(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &imageA);
    tanto_v_TransitionImageLayout(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &imageB);
    tanto_v_TransitionImageLayout(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &imageC);
    tanto_v_TransitionImageLayout(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &imageD);

    tanto_v_ClearColorImage(&imageA);
    tanto_v_ClearColorImage(&imageB);
    tanto_v_ClearColorImage(&imageC);
    tanto_v_ClearColorImage(&imageD);

    tanto_v_TransitionImageLayout(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, &imageA);
    tanto_v_TransitionImageLayout(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, &imageB);
    tanto_v_TransitionImageLayout(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, &imageC);
    tanto_v_TransitionImageLayout(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, &imageD);
}

static void initRenderPasses(void)
{
    tanto_r_CreateRenderPass_ColorDepth(VK_ATTACHMENT_LOAD_OP_CLEAR, 
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 
            tanto_r_GetSwapFormat(), depthFormat, &swapchainRenderPass);
    {
        const VkAttachmentDescription attachmentA = {
            .flags = VK_ATTACHMENT_DESCRIPTION_MAY_ALIAS_BIT,
            .format = textureFormat,
            .samples = VK_SAMPLE_COUNT_1_BIT, // TODO look into what this means
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_GENERAL,
            .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        };

        const VkAttachmentDescription attachmentB = {
            .format = textureFormat,
            .samples = VK_SAMPLE_COUNT_1_BIT, // TODO look into what this means
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };

        const VkAttachmentDescription attachmentC = {
            .format = textureFormat,
            .samples = VK_SAMPLE_COUNT_1_BIT, // TODO look into what this means
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };

        const VkAttachmentDescription attachmentD = {
            .format = textureFormat,
            .samples = VK_SAMPLE_COUNT_1_BIT, // TODO look into what this means
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };

        const VkAttachmentDescription attachmentA2 = {
            .flags = VK_ATTACHMENT_DESCRIPTION_MAY_ALIAS_BIT,
            .format = textureFormat,
            .samples = VK_SAMPLE_COUNT_1_BIT, // TODO look into what this means
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };

        const VkAttachmentReference referenceA1 = {
            .attachment = 0,
            .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        };

        const VkAttachmentReference referenceB1 = {
            .attachment = 1,
            .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
        };

        VkSubpassDescription subpass1 = {
            .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .colorAttachmentCount = 1,
            .pColorAttachments    = &referenceB1,
            .pDepthStencilAttachment = NULL,
            .inputAttachmentCount = 1,
            .pInputAttachments = &referenceA1,
            .preserveAttachmentCount = 0,
        };

        const VkAttachmentReference referenceA2 = {
            .attachment = 4,
            .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
        };

        const VkAttachmentReference referenceB2 = {
            .attachment = 1,
            .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        };

        const VkAttachmentReference referenceC2 = {
            .attachment = 2,
            .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        };

        const VkAttachmentReference referenceD2 = {
            .attachment = 3,
            .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        };

        const VkAttachmentReference inputAttachments[] = {
            referenceB2, referenceC2, referenceD2
        };

        VkSubpassDescription subpass2 = {
            .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .colorAttachmentCount = 1,
            .pColorAttachments    = &referenceA2,
            .pDepthStencilAttachment = NULL,
            .inputAttachmentCount = TANTO_ARRAY_SIZE(inputAttachments),
            .pInputAttachments = inputAttachments,
            .preserveAttachmentCount = 0,
        };

        const VkSubpassDependency dependency1 = {
            .srcSubpass = VK_SUBPASS_EXTERNAL,
            .dstSubpass = 0,
            .srcStageMask = VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        };

        const VkSubpassDependency dependency2 = {
            .srcSubpass = 0,
            .dstSubpass = 1,
            .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT,
        };

        const VkSubpassDependency dependency3 = {
            .srcSubpass = 1,
            .dstSubpass = VK_SUBPASS_EXTERNAL,
            .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        };

        VkSubpassDescription subpasses[] = {
            subpass1, subpass2
        };

        VkSubpassDependency dependencies[] = {
            dependency1, dependency2, dependency3
        };

        VkAttachmentDescription attachments[] = {
            attachmentA, attachmentB, attachmentC, attachmentD, attachmentA2
        };

        VkRenderPassCreateInfo ci = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
            .subpassCount = TANTO_ARRAY_SIZE(subpasses),
            .pSubpasses = subpasses,
            .attachmentCount = TANTO_ARRAY_SIZE(attachments),
            .pAttachments = attachments,
            .dependencyCount = TANTO_ARRAY_SIZE(dependencies),
            .pDependencies = dependencies,
        };

        V_ASSERT( vkCreateRenderPass(device, &ci, NULL, &compositeRenderPass) );
    }

    {
        const VkAttachmentDescription srcAttachment = {
            .flags = VK_ATTACHMENT_DESCRIPTION_MAY_ALIAS_BIT,
            .format = textureFormat,
            .samples = VK_SAMPLE_COUNT_1_BIT, // TODO look into what this means
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        };

        const VkAttachmentDescription dstAttachment = {
            .format = textureFormat,
            .samples = VK_SAMPLE_COUNT_1_BIT, // TODO look into what this means
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        };

        const VkAttachmentReference refSrc = {
            .attachment = 0,
            .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        };

        const VkAttachmentReference refDst = {
            .attachment = 1,
            .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
        };

        const VkSubpassDescription subpass = {
            .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .colorAttachmentCount = 1,
            .pColorAttachments    = &refDst,
            .pDepthStencilAttachment = NULL,
            .inputAttachmentCount = 1,
            .pInputAttachments = &refSrc,
            .preserveAttachmentCount = 0,
        };

        const VkSubpassDependency dependencies[] = {{
            .srcSubpass = VK_SUBPASS_EXTERNAL,
            .dstSubpass = 0,
            .srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
            .srcAccessMask = 0,
            .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        },{
            .srcSubpass = 0,
            .dstSubpass = VK_SUBPASS_EXTERNAL,
            .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        }};

        const VkAttachmentDescription attachments[] = {
            srcAttachment, dstAttachment
        };

        VkRenderPassCreateInfo ci = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
            .subpassCount = 1,
            .pSubpasses = &subpass,
            .attachmentCount = TANTO_ARRAY_SIZE(attachments),
            .pAttachments = attachments,
            .dependencyCount = TANTO_ARRAY_SIZE(dependencies),
            .pDependencies = dependencies,
        };

        V_ASSERT( vkCreateRenderPass(device, &ci, NULL, &singleCompositeRenderPass) );
    }
}

static void initDescSetsAndPipeLayouts(void)
{
    const Tanto_R_DescriptorSetInfo descSets[] = {{
        //   raster
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
            },{ // paint image
                .descriptorCount = 1,
                .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT
            },{
                .descriptorCount = 1,
                .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT
            }}
        },{ // ray trace
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
        },{ // post
            .bindingCount = 1,
            .bindings = {{
                .descriptorCount = 1,
                .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR
            }}
        },{ // apply
            .bindingCount = 1,
            .bindings = {{
                .descriptorCount = 1,
                .type = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
            }}
        },{ // comp
            .bindingCount = 3,
            .bindings = {{
                .descriptorCount = 1,
                .type = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
            },{
                .descriptorCount = 1,
                .type = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
            },{
                .descriptorCount = 1,
                .type = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
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
    },{
        .descriptorSetCount = 2, 
        .descriptorSetLayouts = &descriptorSetLayouts[DESC_SET_APPLY_PAINT]
    }};

    tanto_r_CreatePipelineLayouts(TANTO_ARRAY_SIZE(pipeLayoutInfos), pipeLayoutInfos, pipelineLayouts);
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

    VkDescriptorImageInfo imageInfoAStorage = {
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

    VkDescriptorImageInfo imageInfoC = {
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .imageView   = imageC.view,
        .sampler     = imageC.sampler
    };

    VkDescriptorImageInfo imageInfoD = {
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .imageView   = imageD.view,
        .sampler     = imageD.sampler
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
            .pImageInfo = &imageInfoA
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
            .dstSet = description.descriptorSets[DESC_SET_RAYTRACE],
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .pImageInfo = &imageInfoAStorage
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
        },{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstArrayElement = 0,
            .dstSet = description.descriptorSets[DESC_SET_APPLY_PAINT], 
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
            .pImageInfo = &imageInfoA 
        },{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstArrayElement = 0,
            .dstSet = description.descriptorSets[DESC_SET_COMP], 
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
            .pImageInfo = &imageInfoC
        },{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstArrayElement = 0,
            .dstSet = description.descriptorSets[DESC_SET_COMP], 
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
            .pImageInfo = &imageInfoB
        },{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstArrayElement = 0,
            .dstSet = description.descriptorSets[DESC_SET_COMP], 
            .dstBinding = 2,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
            .pImageInfo = &imageInfoD
    }};

    vkUpdateDescriptorSets(device, TANTO_ARRAY_SIZE(writes), writes, 0, NULL);
}

static void initPipelines(void)
{
    Tanto_R_AttributeSize attrSizes[3] = {12, 12, 12};
    const Tanto_R_GraphicsPipelineInfo pipeInfosGraph[] = {{
        // raster
        .renderPass = swapchainRenderPass, 
        .layout     = pipelineLayouts[LAYOUT_RASTER],
        .vertexDescription = tanto_r_GetVertexDescription(3, attrSizes),
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

static void initPaintPipelines(const Tanto_R_BlendMode blendMode)
{
    assert(blendMode != TANTO_R_BLEND_MODE_NONE);

    const Tanto_R_GraphicsPipelineInfo pipeInfo1 = {
        .layout  = pipelineLayouts[LAYOUT_COMP],
        .renderPass = compositeRenderPass, 
        .subpass = 0,
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .sampleCount = VK_SAMPLE_COUNT_1_BIT,
        .viewportDim = {TEXTURE_SIZE, TEXTURE_SIZE},
        .blendMode   = blendMode,
        .vertShader = tanto_r_FullscreenTriVertShader(),
        .fragShader = SPVDIR"/comp-frag.spv"
    };

    const Tanto_R_GraphicsPipelineInfo pipeInfo2 = {
        .layout  = pipelineLayouts[LAYOUT_COMP],
        .renderPass = compositeRenderPass, 
        .subpass = 1,
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .sampleCount = VK_SAMPLE_COUNT_1_BIT,
        .viewportDim = {TEXTURE_SIZE, TEXTURE_SIZE},
        .blendMode   = blendMode,
        .vertShader = tanto_r_FullscreenTriVertShader(),
        .fragShader = SPVDIR"/comp2-frag.spv"
    };

    const Tanto_R_GraphicsPipelineInfo pipeInfoSingle = {
        .layout  = pipelineLayouts[LAYOUT_COMP],
        .renderPass = singleCompositeRenderPass, 
        .subpass = 0,
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .sampleCount = VK_SAMPLE_COUNT_1_BIT,
        .viewportDim = {TEXTURE_SIZE, TEXTURE_SIZE},
        .blendMode   = blendMode,
        .vertShader = tanto_r_FullscreenTriVertShader(),
        .fragShader = SPVDIR"/comp-frag.spv"
    };

    const Tanto_R_GraphicsPipelineInfo infos[] = {
        pipeInfo1, pipeInfo2, pipeInfoSingle
    };

    tanto_r_CreateGraphicsPipelines(TANTO_ARRAY_SIZE(infos), infos, &graphicsPipelines[PIPELINE_COMP_1]);
}

static void updatePushConstants(void)
{
    rtPushConstants.clearColor = (Vec4){0.1, 0.2, 0.5, 1.0};
    rtPushConstants.lightIntensity = 1.0;
    rtPushConstants.lightDir = (Vec3){-0.707106769, -0.5, -0.5};
    rtPushConstants.lightType = 0;
    rtPushConstants.posOffset =    renderPrim.attrOffsets[0] / sizeof(float);
    rtPushConstants.normalOffset = renderPrim.attrOffsets[1] / sizeof(float);
    rtPushConstants.uvwOffset    = renderPrim.attrOffsets[2] / sizeof(float);
}

static void initSwapchainDependentFramebuffers(void)
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

static void initNonSwapchainDependentFramebuffers(void)
{
    // compositeFrameBuffer
    {
        const VkImageView attachments[] = {
            imageA.view, imageB.view, imageC.view, imageD.view, imageA.view
        };

        VkFramebufferCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .layers = 1,
            .height = TEXTURE_SIZE,
            .width  = TEXTURE_SIZE,
            .renderPass = compositeRenderPass,
            .attachmentCount = 5,
            .pAttachments = attachments
        };

        V_ASSERT( vkCreateFramebuffer(device, &info, NULL, &compositeFrameBuffer) );
    }

    // backgroundFrameBuffer
    {
        const VkImageView attachments[] = {
            imageA.view, imageC.view, 
        };

        VkFramebufferCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .layers = 1,
            .height = TEXTURE_SIZE,
            .width  = TEXTURE_SIZE,
            .renderPass = singleCompositeRenderPass,
            .attachmentCount = 2,
            .pAttachments = attachments
        };

        V_ASSERT( vkCreateFramebuffer(device, &info, NULL, &backgroundFrameBuffer) );
    }

    // foregroundFrameBuffer
    {
        const VkImageView attachments[] = {
            imageA.view, imageD.view, 
        };

        VkFramebufferCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .layers = 1,
            .height = TEXTURE_SIZE,
            .width  = TEXTURE_SIZE,
            .renderPass = singleCompositeRenderPass,
            .attachmentCount = 2,
            .pAttachments = attachments
        };

        V_ASSERT( vkCreateFramebuffer(device, &info, NULL, &foregroundFrameBuffer) );
    }
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

static void onLayerChange(L_LayerId newLayerId)
{
    printf("Begin %s\n", __PRETTY_FUNCTION__);

    vkDeviceWaitIdle(device);

    Tanto_V_Command cmd = tanto_v_CreateCommand(TANTO_V_QUEUE_GRAPHICS_TYPE);

    tanto_v_BeginCommandBuffer(cmd.buffer);

    BufferRegion* prevLayerBuffer = &l_GetLayer(curLayerId)->bufferRegion;

    VkImageSubresourceRange subResRange = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseArrayLayer = 0,
        .baseMipLevel = 0,
        .levelCount = 1,
        .layerCount = 1,
    };

    VkClearColorValue clearColor = {
        .float32[0] = 0,
        .float32[1] = 0,
        .float32[2] = 0,
        .float32[3] = 0,
    };

    VkImageMemoryBarrier barriers[] = {{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .image = imageA.handle,
        .oldLayout = imageA.layout,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .subresourceRange = subResRange,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT
    },{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .image = imageB.handle,
        .oldLayout = imageB.layout,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .subresourceRange = subResRange,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT
    },{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .image = imageC.handle,
        .oldLayout = imageC.layout,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .subresourceRange = subResRange,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT
    },{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .image = imageD.handle,
        .oldLayout = imageD.layout,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .subresourceRange = subResRange,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT
    }};

    vkCmdPipelineBarrier(cmd.buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 
            0, 0, NULL, 0, NULL, TANTO_ARRAY_SIZE(barriers), barriers);

    tanto_v_CmdCopyImageToBuffer(cmd.buffer, &imageB, prevLayerBuffer);

    vkCmdClearColorImage(cmd.buffer, imageC.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1, &subResRange);
    vkCmdClearColorImage(cmd.buffer, imageD.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1, &subResRange);

    VkImageMemoryBarrier barriers0[] = {{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .image = imageC.handle,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .subresourceRange = subResRange,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
    },{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .image = imageD.handle,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .subresourceRange = subResRange,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
    }};

    vkCmdPipelineBarrier(cmd.buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 
            0, 0, NULL, 0, NULL, TANTO_ARRAY_SIZE(barriers0), barriers0);

    curLayerId = newLayerId;

    for (int l = 0; l < curLayerId; l++) 
    {
        BufferRegion* layerBuffer = &l_GetLayer(l)->bufferRegion;

        tanto_v_CmdCopyBufferToImage(cmd.buffer, layerBuffer, &imageA);

        VkClearValue clear = {0.0f, 0.903f, 0.009f, 1.0f};

        const VkRenderPassBeginInfo rpass = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .clearValueCount = 1,
            .pClearValues = &clear,
            .renderArea = {{0, 0}, {TEXTURE_SIZE, TEXTURE_SIZE}},
            .renderPass =  singleCompositeRenderPass,
            .framebuffer = backgroundFrameBuffer,
        };

        vkCmdBeginRenderPass(cmd.buffer, &rpass, VK_SUBPASS_CONTENTS_INLINE);

        vkCmdBindDescriptorSets(
            cmd.buffer, 
            VK_PIPELINE_BIND_POINT_GRAPHICS, 
            pipelineLayouts[LAYOUT_COMP], 
            0, 1, &description.descriptorSets[DESC_SET_APPLY_PAINT], 
            0, NULL);

        vkCmdBindPipeline(cmd.buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipelines[PIPELINE_COMP_SINGLE]);

        vkCmdDraw(cmd.buffer, 3, 1, 0, 0);

        vkCmdEndRenderPass(cmd.buffer);
    }

    const int layerCount = l_GetLayerCount();

    for (int l = curLayerId + 1; l < layerCount; l++) 
    {
        BufferRegion* layerBuffer = &l_GetLayer(l)->bufferRegion;

        tanto_v_CmdCopyBufferToImage(cmd.buffer, layerBuffer, &imageA);

        VkClearValue clear = {0.0f, 0.903f, 0.009f, 1.0f};

        const VkRenderPassBeginInfo rpass = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .clearValueCount = 1,
            .pClearValues = &clear,
            .renderArea = {{0, 0}, {TEXTURE_SIZE, TEXTURE_SIZE}},
            .renderPass =  singleCompositeRenderPass,
            .framebuffer = foregroundFrameBuffer,
        };

        vkCmdBeginRenderPass(cmd.buffer, &rpass, VK_SUBPASS_CONTENTS_INLINE);

        vkCmdBindDescriptorSets(
            cmd.buffer, 
            VK_PIPELINE_BIND_POINT_GRAPHICS, 
            pipelineLayouts[LAYOUT_COMP], 
            0, 1, &description.descriptorSets[DESC_SET_APPLY_PAINT], 
            0, NULL);

        vkCmdBindPipeline(cmd.buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipelines[PIPELINE_COMP_SINGLE]);

        vkCmdDraw(cmd.buffer, 3, 1, 0, 0);

        vkCmdEndRenderPass(cmd.buffer);
    }

    VkImageMemoryBarrier barrier1 = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .image = imageB.handle,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .subresourceRange = subResRange,
        .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT
    };

    vkCmdPipelineBarrier(cmd.buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 
            0, 0, NULL, 0, NULL, 1, &barrier1);

    BufferRegion* layerBuffer = &l_GetLayer(curLayerId)->bufferRegion;

    tanto_v_CmdCopyBufferToImage(cmd.buffer, layerBuffer, &imageB);

    VkImageMemoryBarrier barriers2[] = {{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .image = imageA.handle,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .subresourceRange = subResRange,
        .srcAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT,
        .dstAccessMask = 0
    },{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .image = imageB.handle,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .subresourceRange = subResRange,
        .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT,
        .dstAccessMask = 0 
    },{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .image = imageC.handle,
        .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .subresourceRange = subResRange,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = 0 
    },{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .image = imageD.handle,
        .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .subresourceRange = subResRange,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = 0 
    }};

    vkCmdPipelineBarrier(cmd.buffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 
            0, 0, NULL, 0, NULL, TANTO_ARRAY_SIZE(barriers), barriers2);

    tanto_v_EndCommandBuffer(cmd.buffer);

    tanto_v_SubmitAndWait(&cmd, 0);

    tanto_v_DestroyCommand(cmd);

    printf("End %s\n", __PRETTY_FUNCTION__);
}

static void onRecreateSwapchain(void)
{
    cleanUpSwapchainDependent();

    initOffscreenAttachments();
    initPipelines();
    initSwapchainDependentFramebuffers();
}

static void runUndoCommands(const bool toHost, BufferRegion* bufferRegion)
{
    tanto_v_WaitForFence(&acquireImageCommand.fence);

    tanto_v_ResetCommand(&releaseImageCommand);
    tanto_v_ResetCommand(&transferImageCommand);
    tanto_v_ResetCommand(&acquireImageCommand);

    VkCommandBuffer cmdBuf = releaseImageCommand.buffer;

    tanto_v_BeginCommandBuffer(cmdBuf);

    const VkImageSubresourceRange range = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = 1
    };

    const VkImageLayout otherLayout     = toHost ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    const VkAccessFlags otherAccessMask = toHost ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_TRANSFER_WRITE_BIT;

    VkImageMemoryBarrier imgBarrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = NULL,
        .srcAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT,
        .dstAccessMask = otherAccessMask,
        .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .newLayout = otherLayout,
        .srcQueueFamilyIndex = graphicsQueueFamilyIndex,
        .dstQueueFamilyIndex = transferQueueFamilyIndex,
        .image = imageB.handle,
        .subresourceRange = range
    };

    vkCmdPipelineBarrier(cmdBuf, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 
            VK_DEPENDENCY_BY_REGION_BIT, 0, NULL, 0, NULL, 1, &imgBarrier);

    tanto_v_EndCommandBuffer(cmdBuf);

    cmdBuf = transferImageCommand.buffer;

    tanto_v_BeginCommandBuffer(cmdBuf);

    vkCmdPipelineBarrier(cmdBuf, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 
            VK_DEPENDENCY_BY_REGION_BIT, 0, NULL, 0, NULL, 1, &imgBarrier);

    if (toHost)
        tanto_v_CmdCopyImageToBuffer(cmdBuf, &imageB, bufferRegion);
    else
        tanto_v_CmdCopyBufferToImage(cmdBuf, bufferRegion, &imageB);

    imgBarrier.srcAccessMask = otherAccessMask;
    imgBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    imgBarrier.oldLayout = otherLayout;
    imgBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgBarrier.srcQueueFamilyIndex = transferQueueFamilyIndex;
    imgBarrier.dstQueueFamilyIndex = graphicsQueueFamilyIndex;

    vkCmdPipelineBarrier(cmdBuf, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 
            VK_DEPENDENCY_BY_REGION_BIT, 0, NULL, 0, NULL, 1, &imgBarrier);

    tanto_v_EndCommandBuffer(cmdBuf);

    cmdBuf = acquireImageCommand.buffer;

    tanto_v_BeginCommandBuffer(cmdBuf);

    vkCmdPipelineBarrier(cmdBuf, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 
            VK_DEPENDENCY_BY_REGION_BIT, 0, NULL, 0, NULL, 1, &imgBarrier);

    tanto_v_EndCommandBuffer(cmdBuf);

    tanto_v_SubmitGraphicsCommand(0, NULL, NULL, VK_NULL_HANDLE, &releaseImageCommand);
    
    VkPipelineStageFlags transferStageFlags = VK_PIPELINE_STAGE_TRANSFER_BIT;

    tanto_v_SubmitTransferCommand(0, &transferStageFlags, &releaseImageCommand.semaphore, VK_NULL_HANDLE, &transferImageCommand);

    VkPipelineStageFlags acquireStageFlags = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;

    tanto_v_SubmitGraphicsCommand(0, &acquireStageFlags, &transferImageCommand.semaphore, acquireImageCommand.fence, &acquireImageCommand);
}

static void backupLayer(void)
{
    runUndoCommands(true, u_GetNextBuffer());
    printf("%s\n",__PRETTY_FUNCTION__);
}

static bool undo(void)
{
    printf("%s\n",__PRETTY_FUNCTION__);
    BufferRegion* buf = u_GetLastBuffer();
    if (!buf) return false; // nothing to undo
    runUndoCommands(false, buf);
    return true;
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
    const VkDeviceSize baseAlignment = sbtSelect.bufferRegion.offset;
    const VkDeviceSize rayGenOffset   = baseAlignment + 0;
    const VkDeviceSize missOffset     = baseAlignment + 1u * progSize;
    const VkDeviceSize hitGroupOffset = baseAlignment + 2u * progSize; // have to jump over 1 miss shaders

    assert( rayGenOffset % rtprops.shaderGroupBaseAlignment == 0 );

    VkBufferDeviceAddressInfo addrInfo = {
        .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = sbtSelect.bufferRegion.buffer,
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
    const VkDeviceSize baseAlignment = sbtPaint.bufferRegion.offset;
    const VkDeviceSize rayGenOffset   = baseAlignment + 0;
    const VkDeviceSize missOffset     = baseAlignment + 1u * progSize;
    const VkDeviceSize hitGroupOffset = baseAlignment + 2u * progSize; // have to jump over 1 miss shaders

    assert( rayGenOffset % rtprops.shaderGroupBaseAlignment == 0 );

    VkBufferDeviceAddressInfo addrInfo = {
        .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = sbtPaint.bufferRegion.buffer,
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
            &callableShaderBindingTable, 2000, 
            2000, 1);
}

static void comp(const VkCommandBuffer cmdBuf)
{
    VkClearValue clear = {0, 0, 0, 0};

    VkClearValue clears[] = {
        clear, clear, clear, clear, clear
    };

    const VkRenderPassBeginInfo rpass = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .clearValueCount = TANTO_ARRAY_SIZE(clears),
        .pClearValues = clears,
        .renderArea = {{0, 0}, {TEXTURE_SIZE, TEXTURE_SIZE}},
        .renderPass =  compositeRenderPass,
        .framebuffer = compositeFrameBuffer
    };

    vkCmdBeginRenderPass(cmdBuf, &rpass, VK_SUBPASS_CONTENTS_INLINE);

        vkCmdBindDescriptorSets(
            cmdBuf, 
            VK_PIPELINE_BIND_POINT_GRAPHICS, 
            pipelineLayouts[LAYOUT_COMP], 
            0, 2, &description.descriptorSets[DESC_SET_APPLY_PAINT], 
            0, NULL);

        vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipelines[PIPELINE_COMP_1]);

        vkCmdDraw(cmdBuf, 3, 1, 0, 0);

        vkCmdNextSubpass(cmdBuf, VK_SUBPASS_CONTENTS_INLINE);

        vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipelines[PIPELINE_COMP_2]);

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

    VkCommandBuffer cmdBuf = renderCommands[frameIndex].buffer;

    tanto_v_BeginCommandBuffer(cmdBuf);

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

    comp(cmdBuf);

    rasterize(cmdBuf);

    V_ASSERT( vkEndCommandBuffer(cmdBuf) );
}

void r_InitRenderer(void)
{
    curLayerId = 0;
    needsToBackupLayer = false;
    graphicsQueueFamilyIndex = tanto_v_GetQueueFamilyIndex(TANTO_V_QUEUE_GRAPHICS_TYPE);
    transferQueueFamilyIndex = tanto_v_GetQueueFamilyIndex(TANTO_V_QUEUE_TRANSFER_TYPE);

    for (int i = 0; i < TANTO_FRAME_COUNT; i++) 
    {
        renderCommands[i] = tanto_v_CreateCommand(TANTO_V_QUEUE_GRAPHICS_TYPE);
    }

    releaseImageCommand = tanto_v_CreateCommand(TANTO_V_QUEUE_GRAPHICS_TYPE);
    transferImageCommand = tanto_v_CreateCommand(TANTO_V_QUEUE_TRANSFER_TYPE);
    acquireImageCommand = tanto_v_CreateCommand(TANTO_V_QUEUE_GRAPHICS_TYPE);

    initOffscreenAttachments();
    initPaintImages();

    initRenderPasses();
    initDescSetsAndPipeLayouts();
    initPipelines();
    initPaintPipelines(TANTO_R_BLEND_MODE_OVER);

    tanto_r_CreateShaderBindingTable(3, raytracePipelines[PIPELINE_RAY_TRACE], &sbtPaint);
    tanto_r_CreateShaderBindingTable(3, raytracePipelines[PIPELINE_SELECT], &sbtSelect);
    initNonMeshDescriptors();

    initSwapchainDependentFramebuffers();
    initNonSwapchainDependentFramebuffers();

    tanto_r_RegisterSwapchainRecreationFn(onRecreateSwapchain);

    assert(imageA.size > 0);

    l_Init(imageA.size); // eventually will move this out
    l_RegisterLayerChangeFn(onLayerChange);
    u_InitUndo(imageB.size);
    onLayerChange(0);
}

void r_Render(void)
{
    vkDeviceWaitIdle(device);
    uint32_t i = tanto_r_RequestFrame();
    VkPipelineStageFlags stageFlags = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphore* waitSemaphore = NULL;
    if (needsToBackupLayer)
    {
        backupLayer();
        waitSemaphore = &acquireImageCommand.semaphore;
        needsToBackupLayer = false;
    }
    if (needsToUndo)
    {
        if (undo())
            waitSemaphore = &acquireImageCommand.semaphore;
        needsToUndo = false;
    }
    tanto_v_WaitForFence(&renderCommands[i].fence);
    tanto_v_ResetCommand(&renderCommands[i]);
    updateRenderCommands(i);
    tanto_v_SubmitGraphicsCommand(0, &stageFlags, waitSemaphore, renderCommands[i].fence, &renderCommands[i]);
    vkDeviceWaitIdle(device);
    const VkSemaphore* pWaitSemaphore = tanto_u_Render(&renderCommands[i].semaphore);
    vkDeviceWaitIdle(device);
    tanto_r_PresentFrame(*pWaitSemaphore);
}

int r_GetSelectionPos(Vec3* v)
{
    vkDeviceWaitIdle(device);
    Tanto_V_Command cmd = tanto_v_CreateCommand(TANTO_V_QUEUE_GRAPHICS_TYPE);

    tanto_v_BeginCommandBuffer(cmd.buffer);

    rayTraceSelect(cmd.buffer);

    tanto_v_EndCommandBuffer(cmd.buffer);

    tanto_v_SubmitAndWait(&cmd, 0);

    tanto_v_DestroyCommand(cmd);

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
    tanto_r_BuildBlas(&prim, &bottomLevelAS);
    tanto_r_BuildTlas(&bottomLevelAS, &topLevelAS);

    updatePrimDescriptors();
}

void r_ClearPrim(void)
{
    tanto_r_DestroyAccelerationStruct(&topLevelAS);
    tanto_r_DestroyAccelerationStruct(&bottomLevelAS);
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
        if (pipelineLayouts[i])
            vkDestroyPipelineLayout(device, pipelineLayouts[i], NULL);
    }
    for (int i = 0; i < TANTO_FRAME_COUNT; i++) 
    {
        tanto_v_DestroyCommand(renderCommands[i]);
    }
    memset(graphicsPipelines, 0, sizeof(graphicsPipelines));
    memset(raytracePipelines, 0, sizeof(raytracePipelines));
    memset(&pipelineLayouts, 0, sizeof(pipelineLayouts));
    tanto_v_FreeImage(&imageA);
    tanto_v_FreeImage(&imageB);
    tanto_v_FreeImage(&imageC);
    tanto_v_FreeImage(&imageD);
    vkDestroyDescriptorPool(device, description.descriptorPool, NULL);
    for (int i = 0; i < description.descriptorSetCount; i++) 
    {
        if (descriptorSetLayouts[i])
            vkDestroyDescriptorSetLayout(device, descriptorSetLayouts[i], NULL);
    }
    memset(&description, 0, sizeof(description));
    vkDestroyRenderPass(device, swapchainRenderPass, NULL);
    vkDestroyRenderPass(device, compositeRenderPass, NULL);
    vkDestroyRenderPass(device, singleCompositeRenderPass, NULL);
    vkDestroyFramebuffer(device, compositeFrameBuffer, NULL);
    vkDestroyFramebuffer(device, backgroundFrameBuffer, NULL);
    vkDestroyFramebuffer(device, foregroundFrameBuffer, NULL);
    tanto_v_DestroyCommand(releaseImageCommand);
    tanto_v_DestroyCommand(transferImageCommand);
    tanto_v_DestroyCommand(acquireImageCommand);
    r_ClearPrim();
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
    vkDestroyPipeline(device, graphicsPipelines[PIPELINE_COMP_1], NULL);
    vkDestroyPipeline(device, graphicsPipelines[PIPELINE_COMP_2], NULL);

    Tanto_R_BlendMode blendMode;
    switch (mode)
    {
        case PAINT_MODE_OVER:  blendMode = TANTO_R_BLEND_MODE_OVER;  break;
        case PAINT_MODE_ERASE: blendMode = TANTO_R_BLEND_MODE_ERASE; break;
    }

    initPaintPipelines(blendMode);
    for (int i = 0; i < TANTO_FRAME_COUNT; i++) 
    {
        updateRenderCommands(i);
    }
}

void r_BackUpLayer(void)
{
    needsToBackupLayer = true;
}

void r_Undo(void)
{
    needsToUndo = true;
}
