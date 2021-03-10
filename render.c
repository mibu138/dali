#include "render.h"
#include "coal/m_math.h"
#include "layer.h"
#include "obsidian/r_geo.h"
#include "obsidian/u_ui.h"
#include "obsidian/v_image.h"
#include "obsidian/v_memory.h"
#include <memory.h>
#include <assert.h>
#include <string.h>
#include <obsidian/private.h>
#include <obsidian/v_swapchain.h>
#include <obsidian/v_video.h>
#include <obsidian/t_def.h>
#include <obsidian/t_utils.h>
#include <obsidian/r_pipeline.h>
#include <obsidian/r_raytrace.h>
#include <obsidian/v_command.h>
#include <obsidian/v_video.h>
#include <obsidian/r_renderpass.h>
#include <vulkan/vulkan_core.h>
#include "undo.h"
#include "painter.h"
#include <stdlib.h>

#include "ubo-shared.h"

#include <pthread.h>

#define SPVDIR "/home/michaelb/dev/painter/shaders/spv"

typedef Obdn_V_Command Command;
typedef Obdn_V_Image   Image;

enum {
    LAYOUT_RASTER,
    LAYOUT_RAYTRACE,
    LAYOUT_POST,
    LAYOUT_COMP,
    LAYOUT_COUNT
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
    PIPELINE_COMP_1,
    PIPELINE_COMP_2,
    PIPELINE_COMP_3,
    PIPELINE_COMP_4,
    PIPELINE_COMP_SINGLE,
    G_PIPELINE_COUNT
};

_Static_assert(G_PIPELINE_COUNT < OBDN_MAX_PIPELINES, "must be less than max pipelines");

enum {
    PIPELINE_PAINT,
    PIPELINE_SELECT,
    RT_PIPELINE_COUNT
};

typedef Obdn_V_BufferRegion BufferRegion;

static BufferRegion  matrixRegion;
static BufferRegion  brushRegion;
static BufferRegion  selectionRegion;

static Obdn_R_Primitive     renderPrim;

static VkPipelineLayout           pipelineLayouts[OBDN_MAX_PIPELINES];
static VkPipeline                 graphicsPipelines[G_PIPELINE_COUNT];

static VkDescriptorSetLayout descriptorSetLayouts[OBDN_MAX_DESCRIPTOR_SETS];
static Obdn_R_Description   description;

static Obdn_V_Image   depthAttachment;

static uint32_t graphicsQueueFamilyIndex;

static Command renderCommand;

static VkFramebuffer   swapchainFrameBuffers[OBDN_FRAME_COUNT];
static VkFramebuffer   postFrameBuffers[OBDN_FRAME_COUNT];

static const VkFormat depthFormat   = VK_FORMAT_D32_SFLOAT;

static VkRenderPass swapchainRenderPass;
static VkRenderPass postRenderPass;

static const Scene* scene;

extern Parms parms;
// swap to host stuff

static bool            copySwapToHost;
static bool            fastPath;
static BufferRegion    swapHostBufferColor;
static BufferRegion    swapHostBufferDepth;
static Command         copyToHostCommand;
static VkSemaphore     extFrameReadSemaphore;

// swap to host stuff

static void updateRenderCommands(const int8_t frameIndex);
static void initOffscreenAttachments(void);
static void updatePrimDescriptors(void);
static void initNonMeshDescriptors(void);
static void initDescSetsAndPipeLayouts(void);
static void rayTraceSelect(const VkCommandBuffer cmdBuf);
static void rasterize(const VkCommandBuffer cmdBuf);
static void cleanUpSwapchainDependent(void);
static void updateRenderCommands(const int8_t frameIndex);

static void initOffscreenAttachments(void)
{
    Obdn_V_MemoryType memType = parms.copySwapToHost ? OBDN_V_MEMORY_EXTERNAL_DEVICE_TYPE : OBDN_V_MEMORY_DEVICE_TYPE;
    depthAttachment = obdn_v_CreateImage(
            OBDN_WINDOW_WIDTH, OBDN_WINDOW_HEIGHT,
            depthFormat,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_ASPECT_DEPTH_BIT, 
            VK_SAMPLE_COUNT_1_BIT,
            1,
            memType);
}

static void initRenderPasses(void)
{
    obdn_r_CreateRenderPass_ColorDepth(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, 
            VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
            obdn_v_GetSwapFormat(), depthFormat, &swapchainRenderPass);

    obdn_r_CreateRenderPass_Color(
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ATTACHMENT_LOAD_OP_LOAD, obdn_v_GetSwapFormat(), &postRenderPass);

    // apply paint renderpass
    {
        const VkAttachmentDescription attachmentA = {
            .format        = textureFormat,
            .samples       = VK_SAMPLE_COUNT_1_BIT,
            .loadOp        = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp       = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_GENERAL,
            .finalLayout   = VK_IMAGE_LAYOUT_GENERAL,
        };

        const VkAttachmentDescription attachmentB = {
            .format        = textureFormat,
            .samples       = VK_SAMPLE_COUNT_1_BIT,
            .loadOp        = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp       = VK_ATTACHMENT_STORE_OP_STORE,
            .initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .finalLayout   = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
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
            .pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .colorAttachmentCount    = 1,
            .pColorAttachments       = &referenceB1,
            .pDepthStencilAttachment = NULL,
            .inputAttachmentCount    = 1,
            .pInputAttachments       = &referenceA1,
            .preserveAttachmentCount = 0,
        };

        const VkSubpassDependency dependency1 = {
            .srcSubpass    = VK_SUBPASS_EXTERNAL,
            .dstSubpass    = 0,
            .srcStageMask  = VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        };

        const VkSubpassDependency dependency2 = {
            .srcSubpass    = 0,
            .dstSubpass    = VK_SUBPASS_EXTERNAL,
            .srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        };

        VkSubpassDependency dependencies[] = {
            dependency1, dependency2
        };

        VkAttachmentDescription attachments[] = {
            attachmentA, attachmentB
        };

        VkRenderPassCreateInfo ci = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
            .subpassCount = 1,
            .pSubpasses = &subpass1,
            .attachmentCount = OBDN_ARRAY_SIZE(attachments),
            .pAttachments = attachments,
            .dependencyCount = OBDN_ARRAY_SIZE(dependencies),
            .pDependencies = dependencies,
        };

        V_ASSERT( vkCreateRenderPass(device, &ci, NULL, &applyPaintRenderPass) );
    }

    // comp renderpass
    {
        const VkAttachmentDescription attachmentB = {
            .format        = textureFormat,
            .samples       = VK_SAMPLE_COUNT_1_BIT,
            .loadOp        = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp       = VK_ATTACHMENT_STORE_OP_STORE,
            .initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .finalLayout   = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };

        const VkAttachmentDescription attachmentC = {
            .format        = textureFormat,
            .samples       = VK_SAMPLE_COUNT_1_BIT,
            .loadOp        = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp       = VK_ATTACHMENT_STORE_OP_STORE,
            .initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .finalLayout   = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };

        const VkAttachmentDescription attachmentD = {
            .format        = textureFormat,
            .samples       = VK_SAMPLE_COUNT_1_BIT,
            .loadOp        = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp       = VK_ATTACHMENT_STORE_OP_STORE,
            .initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .finalLayout   = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };

        const VkAttachmentDescription attachmentA2 = {
            .format        = textureFormat,
            .samples       = VK_SAMPLE_COUNT_1_BIT,
            .loadOp        = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp       = VK_ATTACHMENT_STORE_OP_STORE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout   = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };

        const VkAttachmentReference referenceB2 = {
            .attachment = 0,
            .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        };

        const VkAttachmentReference referenceC2 = {
            .attachment = 1,
            .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        };

        const VkAttachmentReference referenceD2 = {
            .attachment = 2,
            .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        };

        const VkAttachmentReference referenceA2 = {
            .attachment = 3,
            .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
        };

        VkSubpassDescription subpass1 = {
            .pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .colorAttachmentCount    = 1,
            .pColorAttachments       = &referenceA2,
            .pDepthStencilAttachment = NULL,
            .inputAttachmentCount    = 1,
            .pInputAttachments       = &referenceC2,
            .preserveAttachmentCount = 0,
        };

        VkSubpassDescription subpass2 = {
            .pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .colorAttachmentCount    = 1,
            .pColorAttachments       = &referenceA2,
            .pDepthStencilAttachment = NULL,
            .inputAttachmentCount    = 1,
            .pInputAttachments       = &referenceB2,
            .preserveAttachmentCount = 0,
        };

        VkSubpassDescription subpass3 = {
            .pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .colorAttachmentCount    = 1,
            .pColorAttachments       = &referenceA2,
            .pDepthStencilAttachment = NULL,
            .inputAttachmentCount    = 1,
            .pInputAttachments       = &referenceD2,
            .preserveAttachmentCount = 0,
        };

        const VkSubpassDependency dependency1 = {
            .srcSubpass    = VK_SUBPASS_EXTERNAL,
            .dstSubpass    = 0,
            .srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT,
        };

        const VkSubpassDependency dependency2 = {
            .srcSubpass    = 0,
            .dstSubpass    = 1,
            .srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT,
        };

        const VkSubpassDependency dependency3 = {
            .srcSubpass    = 1,
            .dstSubpass    = 2,
            .srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT,
        };

        const VkSubpassDependency dependency4 = {
            .srcSubpass    = 2,
            .dstSubpass    = VK_SUBPASS_EXTERNAL,
            .srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        };

        VkSubpassDescription subpasses[] = {
            subpass1, subpass2, subpass3, 
        };

        VkSubpassDependency dependencies[] = {
            dependency1, dependency2, dependency3, dependency4, 
        };

        VkAttachmentDescription attachments[] = {
           attachmentB, attachmentC, attachmentD, attachmentA2
        };

        VkRenderPassCreateInfo ci = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
            .subpassCount = OBDN_ARRAY_SIZE(subpasses),
            .pSubpasses = subpasses,
            .attachmentCount = OBDN_ARRAY_SIZE(attachments),
            .pAttachments = attachments,
            .dependencyCount = OBDN_ARRAY_SIZE(dependencies),
            .pDependencies = dependencies,
        };

        V_ASSERT( vkCreateRenderPass(device, &ci, NULL, &compositeRenderPass) );
    }

    {
        const VkAttachmentDescription srcAttachment = {
            .format        = textureFormat,
            .samples       = VK_SAMPLE_COUNT_1_BIT,
            .loadOp        = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp       = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .finalLayout   = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        };

        const VkAttachmentDescription dstAttachment = {
            .format        = textureFormat,
            .samples       = VK_SAMPLE_COUNT_1_BIT,
            .loadOp        = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp       = VK_ATTACHMENT_STORE_OP_STORE,
            .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .finalLayout   = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
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
            .pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .colorAttachmentCount    = 1,
            .pColorAttachments       = &refDst,
            .pDepthStencilAttachment = NULL,
            .inputAttachmentCount    = 1,
            .pInputAttachments       = &refSrc,
            .preserveAttachmentCount = 0,
        };

        const VkSubpassDependency dependencies[] = {{
            .srcSubpass    = VK_SUBPASS_EXTERNAL,
            .dstSubpass    = 0,
            .srcStageMask  = VK_PIPELINE_STAGE_TRANSFER_BIT,
            .srcAccessMask = 0,
            .dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        },{
            .srcSubpass    = 0,
            .dstSubpass    = VK_SUBPASS_EXTERNAL,
            .srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstStageMask  = VK_PIPELINE_STAGE_TRANSFER_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        }};

        const VkAttachmentDescription attachments[] = {
            srcAttachment, dstAttachment
        };

        VkRenderPassCreateInfo ci = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
            .subpassCount = 1,
            .pSubpasses = &subpass,
            .attachmentCount = OBDN_ARRAY_SIZE(attachments),
            .pAttachments = attachments,
            .dependencyCount = OBDN_ARRAY_SIZE(dependencies),
            .pDependencies = dependencies,
        };

        V_ASSERT( vkCreateRenderPass(device, &ci, NULL, &singleCompositeRenderPass) );
    }
}

static void initDescSetsAndPipeLayouts(void)
{
    const Obdn_R_DescriptorSetInfo descSets[] = {{
        //   raster
            .bindingCount = 4, 
            .bindings = {{
                .descriptorCount = 1,
                .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .stageFlags = VK_SHADER_STAGE_VERTEX_BIT
            },{ // paint image
                .descriptorCount = 1,
                .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT
            }}
        },{ // post
            .bindingCount = 1,
            .bindings = {{
                .descriptorCount = 1,
                .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT
            }}
        }
    };

    obdn_r_CreateDescriptorSetLayouts(OBDN_ARRAY_SIZE(descSets), descSets, descriptorSetLayouts);

    obdn_r_CreateDescriptorSets(OBDN_ARRAY_SIZE(descSets), descSets, descriptorSetLayouts, &description);

    const Obdn_R_PipelineLayoutInfo pipeLayoutInfos[] = {{
        .descriptorSetCount = 2, 
        .descriptorSetLayouts = descriptorSetLayouts,
    }};

    obdn_r_CreatePipelineLayouts(OBDN_ARRAY_SIZE(pipeLayoutInfos), pipeLayoutInfos, pipelineLayouts);
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

    VkDescriptorBufferInfo uvBufInfo = {
        .offset = renderPrim.vertexRegion.offset + renderPrim.attrOffsets[2],
        .range  = renderPrim.vertexRegion.size - renderPrim.attrOffsets[2], //works only because uv is the last attribute
        .buffer = renderPrim.vertexRegion.buffer,
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
        },{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstArrayElement = 0,
            .dstSet = description.descriptorSets[DESC_SET_RAYTRACE],
            .dstBinding = 3,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &uvBufInfo
    }};

    vkUpdateDescriptorSets(device, OBDN_ARRAY_SIZE(writes), writes, 0, NULL);
}

static void initNonMeshDescriptors(void)
{
    matrixRegion = obdn_v_RequestBufferRegion(sizeof(UboMatrices), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            OBDN_V_MEMORY_HOST_GRAPHICS_TYPE);
    UboMatrices* matrices = (UboMatrices*)matrixRegion.hostData;
    matrices->model   = m_Ident_Mat4();
    matrices->view    = m_Ident_Mat4();
    matrices->proj    = m_Ident_Mat4();
    matrices->viewInv = m_Ident_Mat4();
    matrices->projInv = m_Ident_Mat4();

    VkDescriptorBufferInfo uniformInfoMatrices = {
        .range  = matrixRegion.size,
        .offset = matrixRegion.offset,
        .buffer = matrixRegion.buffer,
    };

    VkWriteDescriptorSet writes[] = {{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstArrayElement = 0,
            .dstSet = description.descriptorSets[DESC_SET_RASTER],
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .pBufferInfo = &uniformInfoMatrices
    }};

    vkUpdateDescriptorSets(device, OBDN_ARRAY_SIZE(writes), writes, 0, NULL);
}

static void initRasterPipelines(void)
{
    Obdn_R_AttributeSize attrSizes[3] = {12, 12, 8};
    const Obdn_R_GraphicsPipelineInfo pipeInfosGraph[] = {{
        // raster
        .renderPass = swapchainRenderPass, 
        .layout     = pipelineLayouts[LAYOUT_RASTER],
        .vertexDescription = obdn_r_GetVertexDescription(3, attrSizes),
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .sampleCount = VK_SAMPLE_COUNT_1_BIT,
        .viewportDim = {OBDN_WINDOW_WIDTH, OBDN_WINDOW_HEIGHT},
        .vertShader = SPVDIR"/raster-vert.spv", 
        .fragShader = SPVDIR"/raster-frag.spv"
    },{
        // post
        .renderPass = postRenderPass,
        .layout     = pipelineLayouts[LAYOUT_POST],
        .sampleCount = VK_SAMPLE_COUNT_1_BIT,
        .frontFace   = VK_FRONT_FACE_CLOCKWISE,
        .blendMode   = OBDN_R_BLEND_MODE_OVER,
        .viewportDim = {OBDN_WINDOW_WIDTH, OBDN_WINDOW_HEIGHT},
        .vertShader = obdn_r_FullscreenTriVertShader(),
        .fragShader = SPVDIR"/post-frag.spv"
    }};

    obdn_r_CreateGraphicsPipelines(OBDN_ARRAY_SIZE(pipeInfosGraph), pipeInfosGraph, graphicsPipelines);
}

static void destroyPaintPipelines(void)
{
    for (int i = PIPELINE_COMP_1; i < G_PIPELINE_COUNT; i++)
    {
        vkDestroyPipeline(device, graphicsPipelines[i], NULL);
    }
}

static void initSwapchainDependentFramebuffers(void)
{
    for (int i = 0; i < OBDN_FRAME_COUNT; i++) 
    {
        const Obdn_R_Frame* frame = obdn_v_GetFrame(i);

        const VkImageView offscreenAttachments[] = {frame->view, depthAttachment.view};

        VkFramebufferCreateInfo framebufferInfo = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .layers = 1,
            .height = OBDN_WINDOW_HEIGHT,
            .width  = OBDN_WINDOW_WIDTH,
            .renderPass = swapchainRenderPass,
            .attachmentCount = 2,
            .pAttachments = offscreenAttachments 
        };

        V_ASSERT( vkCreateFramebuffer(device, &framebufferInfo, NULL, &swapchainFrameBuffers[i]) );

        framebufferInfo.renderPass = postRenderPass;
        framebufferInfo.attachmentCount = 1;

        V_ASSERT( vkCreateFramebuffer(device, &framebufferInfo, NULL, &postFrameBuffers[i]) );
    }
}

static void cleanUpSwapchainDependent(void)
{
    for (int i = 0; i < OBDN_FRAME_COUNT; i++) 
    {
        vkDestroyFramebuffer(device, swapchainFrameBuffers[i], NULL);
        vkDestroyFramebuffer(device, postFrameBuffers[i], NULL);
    }
    vkDestroyPipeline(device, graphicsPipelines[PIPELINE_RASTER], NULL);
    vkDestroyPipeline(device, graphicsPipelines[PIPELINE_POST], NULL);
    obdn_v_FreeImage(&depthAttachment);
    if (copySwapToHost)
    {
        obdn_v_FreeBufferRegion(&swapHostBufferColor);
        obdn_v_FreeBufferRegion(&swapHostBufferDepth);
    }
}

static void onRecreateSwapchain(void)
{
    cleanUpSwapchainDependent();

    initOffscreenAttachments();
    initRasterPipelines();
    initSwapchainDependentFramebuffers();

    if (copySwapToHost)
    {
        const uint64_t size = obdn_v_GetFrame(obdn_v_GetCurrentFrameIndex())->size;
        swapHostBufferColor = obdn_v_RequestBufferRegion(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, OBDN_V_MEMORY_HOST_GRAPHICS_TYPE);
        swapHostBufferDepth = obdn_v_RequestBufferRegion(depthAttachment.size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, OBDN_V_MEMORY_HOST_GRAPHICS_TYPE);
    }
}

static void updateView(void)
{
    UboMatrices* matrices = (UboMatrices*)matrixRegion.hostData;
    matrices->view = scene->view;
    matrices->viewInv = m_Invert4x4(&scene->view);
}

static void updateProj(void)
{
    UboMatrices* matrices = (UboMatrices*)matrixRegion.hostData;
    matrices->proj = scene->proj;
    matrices->projInv = m_Invert4x4(&scene->proj);
}

static VkSemaphore syncScene(const uint32_t frameIndex)
{
    VkSemaphore semaphore = VK_NULL_HANDLE;
    if (scene->dirt)
    {
        if (scene->dirt & SCENE_VIEW_BIT)
            updateView();
        if (scene->dirt & SCENE_PROJ_BIT)
            updateProj();
        if (scene->dirt & SCENE_BRUSH_BIT)
            updateBrush();
        if (scene->dirt & SCENE_WINDOW_BIT)
        {
            OBDN_WINDOW_WIDTH = scene->window_width;
            OBDN_WINDOW_HEIGHT = scene->window_height;
            obdn_v_RecreateSwapchain();
        }
        if (scene->dirt & SCENE_UNDO_BIT)
        {
            if (undo())
                semaphore = acquireImageCommand.semaphore;
        }
        if (scene->dirt & SCENE_LAYER_CHANGED_BIT)
        {
            onLayerChange(scene->layer);
        }
        if (scene->dirt & SCENE_LAYER_BACKUP_BIT)
        {
            backupLayer();
            semaphore = acquireImageCommand.semaphore;
        }
        if (scene->dirt & SCENE_PAINT_MODE_BIT)
        {
            updatePaintMode();
        }
    }
    return semaphore;
}

static void rayTraceSelect(const VkCommandBuffer cmdBuf)
{
    vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, raytracePipelines[PIPELINE_SELECT]); 

    vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, 
            pipelineLayouts[LAYOUT_RAYTRACE], 0, 3, description.descriptorSets, 0, NULL);

    vkCmdTraceRaysKHR(cmdBuf, 
            &shaderBindingTables[PIPELINE_SELECT].raygenTable,
            &shaderBindingTables[PIPELINE_SELECT].missTable,
            &shaderBindingTables[PIPELINE_SELECT].hitTable,
            &shaderBindingTables[PIPELINE_SELECT].callableTable,
            1, 1, 1);
}

static void rasterize(const VkCommandBuffer cmdBuf)
{
    VkClearValue clearValueColor =     {0.0f, 0.0f, 0.0f, 0.0f};
    VkClearValue clearValueDepth = {1.0, 0};

    VkClearValue clears[] = {clearValueColor, clearValueDepth};

    vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipelines[PIPELINE_RASTER]);

    vkCmdBindDescriptorSets(
        cmdBuf, 
        VK_PIPELINE_BIND_POINT_GRAPHICS, 
        pipelineLayouts[LAYOUT_RASTER], 
        0, 1, &description.descriptorSets[DESC_SET_RASTER], 
        0, NULL);

    {
        const VkRenderPassBeginInfo rpass = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .clearValueCount = 2,
            .pClearValues = clears,
            .renderArea = {{0, 0}, {OBDN_WINDOW_WIDTH, OBDN_WINDOW_HEIGHT}},
            .renderPass =  swapchainRenderPass,
            .framebuffer = swapchainFrameBuffers[obdn_v_GetCurrentFrameIndex()]
        };

        vkCmdBeginRenderPass(cmdBuf, &rpass, VK_SUBPASS_CONTENTS_INLINE);

            obdn_r_DrawPrim(cmdBuf, &renderPrim);
            
        vkCmdEndRenderPass(cmdBuf);
    }
    {
        const VkRenderPassBeginInfo rpass = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .clearValueCount = 1,
            .pClearValues = &clearValueColor,
            .renderArea = {{0, 0}, {OBDN_WINDOW_WIDTH, OBDN_WINDOW_HEIGHT}},
            .renderPass =  postRenderPass,
            .framebuffer = postFrameBuffers[obdn_v_GetCurrentFrameIndex()]
        };

        vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipelines[PIPELINE_POST]);

        vkCmdBindDescriptorSets(
            cmdBuf, 
            VK_PIPELINE_BIND_POINT_GRAPHICS, 
            pipelineLayouts[LAYOUT_POST], 
            0, 1, &description.descriptorSets[DESC_SET_POST],
            0, NULL);

        vkCmdBeginRenderPass(cmdBuf, &rpass, VK_SUBPASS_CONTENTS_INLINE);

            vkCmdDraw(cmdBuf, 3, 1, 0, 0);

        vkCmdEndRenderPass(cmdBuf);
    }
}

static void updateRenderCommands(const int8_t frameIndex)
{
    VkCommandBuffer cmdBuf = renderCommands[frameIndex].buffer;

    obdn_v_BeginCommandBuffer(cmdBuf);

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

    if (brushActive)
    {
        static const float unit = 0.0025; //in screen space
        const float brushDist = m_Distance(brushPos, prevBrushPos);
        const int splatCount = MIN(MAX(brushDist / unit, 1), 8);
        for (int i = 0; i < splatCount; i++)
        {
            float t = (float)i / splatCount;
            float xstep = t * (brushPos.x - prevBrushPos.x);
            float ystep = t * (brushPos.y - prevBrushPos.y);
            float x = prevBrushPos.x + xstep;
            float y = prevBrushPos.y + ystep;

            paint(cmdBuf, x, y);

            applyPaint(cmdBuf);

            vkCmdClearColorImage(cmdBuf, imageA.handle, VK_IMAGE_LAYOUT_GENERAL, &clearColor, 1, &imageRange);
        }
    }
    else
        applyPaint(cmdBuf);

    comp(cmdBuf);

    rasterize(cmdBuf);

    V_ASSERT( vkEndCommandBuffer(cmdBuf) );
}

void r_InitRenderer(const uint32_t texSize)
{
    assert(texSize > 0);
    assert(texSize % 256 == 0);
    assert(texSize == IMG_4K || texSize == IMG_8K || texSize == IMG_16K); // for now

    textureSize = texSize;
    curLayerId = 0;
    graphicsQueueFamilyIndex = obdn_v_GetQueueFamilyIndex(OBDN_V_QUEUE_GRAPHICS_TYPE);
    transferQueueFamilyIndex = obdn_v_GetQueueFamilyIndex(OBDN_V_QUEUE_TRANSFER_TYPE);

    copySwapToHost = parms.copySwapToHost; //note this

    for (int i = 0; i < OBDN_FRAME_COUNT; i++) 
    {
        renderCommands[i] = obdn_v_CreateCommand(OBDN_V_QUEUE_GRAPHICS_TYPE);
    }

    releaseImageCommand = obdn_v_CreateCommand(OBDN_V_QUEUE_GRAPHICS_TYPE);
    transferImageCommand = obdn_v_CreateCommand(OBDN_V_QUEUE_TRANSFER_TYPE);
    acquireImageCommand = obdn_v_CreateCommand(OBDN_V_QUEUE_GRAPHICS_TYPE);

    initOffscreenAttachments();
    initPaintImages();

    initRenderPasses();
    initDescSetsAndPipeLayouts();
    initRasterPipelines();
    initRayTracePipelinesAndShaderBindingTables();
    initPaintPipelines(OBDN_R_BLEND_MODE_OVER);

    initNonMeshDescriptors();

    initSwapchainDependentFramebuffers();
    initNonSwapchainDependentFramebuffers();

    obdn_v_RegisterSwapchainRecreationFn(onRecreateSwapchain);

    assert(imageA.size > 0);

    l_Init(imageA.size); // eventually will move this out
    uint8_t maxUndoStacks, maxUndosPerStack;
    switch (texSize)
    {
        case IMG_4K:  maxUndoStacks = 4; maxUndosPerStack = 8; break;
        case IMG_8K:  maxUndoStacks = 2; maxUndosPerStack = 8; break;
        case IMG_16K: maxUndoStacks = 1; maxUndosPerStack = 8; break;
        default: abort();
    }
    u_InitUndo(imageA.size, maxUndoStacks, maxUndosPerStack);
    onLayerChange(0);
    
    if (copySwapToHost)
    {
        VkDeviceSize swapImageSize = obdn_v_GetFrame(0)->size;
        swapHostBufferColor = obdn_v_RequestBufferRegion(swapImageSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT, OBDN_V_MEMORY_HOST_GRAPHICS_TYPE);
        swapHostBufferDepth = obdn_v_RequestBufferRegion(depthAttachment.size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, OBDN_V_MEMORY_HOST_GRAPHICS_TYPE);
        copyToHostCommand = obdn_v_CreateCommand(OBDN_V_QUEUE_GRAPHICS_TYPE);
        VkSemaphoreCreateInfo semCI = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        vkCreateSemaphore(device, &semCI, NULL, &extFrameReadSemaphore);
        fastPath = false;
        printf(">> SwapHostBuffer created\n");
    }
}

void r_Render(void)
{
    Obdn_Mask dummyMask;
    uint32_t i = obdn_v_RequestFrame(&dummyMask);

    VkSemaphore waitSemaphore = syncScene(i);
    obdn_v_WaitForFence(&renderCommands[i].fence);
    obdn_v_ResetCommand(&renderCommands[i]);
    updateRenderCommands(i);
    obdn_v_SubmitGraphicsCommand(0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 
            waitSemaphore, renderCommands[i].semaphore, 
            renderCommands[i].fence, renderCommands[i].buffer);
    waitSemaphore = obdn_u_Render(renderCommands[i].semaphore);
    if (copySwapToHost)
    {
        if (fastPath)
        { // this command only serves to signal the semaphore to avoid validation errors.
            obdn_v_ResetCommand(&copyToHostCommand);
            obdn_v_BeginCommandBuffer(copyToHostCommand.buffer);
            obdn_v_EndCommandBuffer(copyToHostCommand.buffer);
            obdn_v_SubmitGraphicsCommand(0, VK_PIPELINE_STAGE_TRANSFER_BIT, waitSemaphore, 
                    VK_NULL_HANDLE,
                    copyToHostCommand.fence, 
                    copyToHostCommand.buffer);
            obdn_v_WaitForFence(&copyToHostCommand.fence);
        }
        else
        {
            obdn_v_ResetCommand(&copyToHostCommand);
            obdn_v_BeginCommandBuffer(copyToHostCommand.buffer);

            const Image* swapImage = obdn_v_GetFrame(i);

            assert(swapImage->size == swapHostBufferColor.size);

            obdn_v_CmdCopyImageToBuffer(copyToHostCommand.buffer, swapImage, VK_IMAGE_ASPECT_COLOR_BIT, &swapHostBufferColor);
            obdn_v_CmdCopyImageToBuffer(copyToHostCommand.buffer, &depthAttachment, VK_IMAGE_ASPECT_DEPTH_BIT, &swapHostBufferDepth);

            obdn_v_EndCommandBuffer(copyToHostCommand.buffer);

            obdn_v_SubmitGraphicsCommand(0, VK_PIPELINE_STAGE_TRANSFER_BIT, waitSemaphore, 
                    VK_NULL_HANDLE,
                    copyToHostCommand.fence, 
                    copyToHostCommand.buffer);

            obdn_v_WaitForFence(&copyToHostCommand.fence);
        }
    }
    else   
        obdn_v_PresentFrame(waitSemaphore);
    obdn_v_WaitForFenceNoReset(&renderCommands[i].fence);
}

int r_GetSelectionPos(Vec3* v)
{
    Obdn_V_Command cmd = obdn_v_CreateCommand(OBDN_V_QUEUE_GRAPHICS_TYPE);

    obdn_v_BeginCommandBuffer(cmd.buffer);

    rayTraceSelect(cmd.buffer);

    obdn_v_EndCommandBuffer(cmd.buffer);

    obdn_v_SubmitAndWait(&cmd, 0);

    obdn_v_DestroyCommand(cmd);

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

void r_BindScene(const Scene* scene_)
{
    scene = scene_;
    printf("Scene bound!\n");
}

void r_LoadPrim(Obdn_R_Primitive prim)
{
    assert(renderPrim.vertexRegion.size == 0);
    renderPrim = prim;
    obdn_r_BuildBlas(&prim, &bottomLevelAS);
    obdn_r_BuildTlas(&bottomLevelAS, &topLevelAS);

    updatePrimDescriptors();
}

void r_ClearPrim(void)
{
    obdn_r_DestroyAccelerationStruct(&topLevelAS);
    obdn_r_DestroyAccelerationStruct(&bottomLevelAS);
    obdn_r_FreePrim(&renderPrim);
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
    OBDN_DEBUG_PRINT("%s", ext);
    Obdn_V_ImageFileType fileType;
    if (strncmp(ext, "png", 3) == 0) fileType = OBDN_V_IMAGE_FILE_TYPE_PNG;
    else if (strncmp(ext, "jpg", 3) == 0) fileType = OBDN_V_IMAGE_FILE_TYPE_JPG;
    else 
    {
        printf("Bad extension.\n");
        return;
    }
    obdn_v_SaveImage(&imageA, fileType, strbuf);
}

void r_ClearPaintImage(void)
{
    printf("called %s. need to reimplement.\n", __PRETTY_FUNCTION__);
    //obdn_v_ClearColorImage(&paintImage);
}

void r_CleanUp(void)
{
    cleanUpSwapchainDependent();
    obdn_v_UnregisterSwapchainRecreateFn(onRecreateSwapchain);
    for (int i = PIPELINE_COMP_1; i < G_PIPELINE_COUNT; i++)  // first 2 handles in swapcleanup
    {
        vkDestroyPipeline(device, graphicsPipelines[i], NULL);
    }
    for (int i = 0; i < RT_PIPELINE_COUNT; i++)
    {
        obdn_r_DestroyShaderBindingTable(&shaderBindingTables[i]);
        vkDestroyPipeline(device, raytracePipelines[i], NULL);
    }
    for (int i = 0; i < LAYOUT_COUNT; i++)
    {
        vkDestroyPipelineLayout(device, pipelineLayouts[i], NULL);
    }
    for (int i = 0; i < OBDN_FRAME_COUNT; i++) 
    {
        obdn_v_DestroyCommand(renderCommands[i]);
    }
    memset(graphicsPipelines, 0, sizeof(graphicsPipelines));
    memset(raytracePipelines, 0, sizeof(raytracePipelines));
    memset(&pipelineLayouts, 0, sizeof(pipelineLayouts));
    obdn_v_FreeImage(&imageA);
    obdn_v_FreeImage(&imageB);
    obdn_v_FreeImage(&imageC);
    obdn_v_FreeImage(&imageD);
    for (int i = 0; i < description.descriptorSetCount; i++) 
    {
        if (descriptorSetLayouts[i])
            vkDestroyDescriptorSetLayout(device, descriptorSetLayouts[i], NULL);
    }
    obdn_r_DestroyDescription(&description);
    memset(&description, 0, sizeof(description));
    vkDestroyRenderPass(device, swapchainRenderPass, NULL);
    vkDestroyRenderPass(device, postRenderPass, NULL);
    vkDestroyRenderPass(device, compositeRenderPass, NULL);
    vkDestroyRenderPass(device, singleCompositeRenderPass, NULL);
    vkDestroyRenderPass(device, applyPaintRenderPass, NULL);
    vkDestroyFramebuffer(device, compositeFrameBuffer, NULL);
    vkDestroyFramebuffer(device, backgroundFrameBuffer, NULL);
    vkDestroyFramebuffer(device, foregroundFrameBuffer, NULL);
    vkDestroyFramebuffer(device, applyPaintFrameBuffer, NULL);
    obdn_v_DestroyCommand(releaseImageCommand);
    obdn_v_DestroyCommand(transferImageCommand);
    obdn_v_DestroyCommand(acquireImageCommand);
    if (copySwapToHost)
    {
        vkDestroySemaphore(device, extFrameReadSemaphore, NULL);
        obdn_v_DestroyCommand(copyToHostCommand);
    }
    obdn_v_FreeBufferRegion(&matrixRegion);
    obdn_v_FreeBufferRegion(&brushRegion);
    obdn_v_FreeBufferRegion(&selectionRegion);
    r_ClearPrim();
    l_CleanUp();
    u_CleanUp();
}

void r_GetSwapBufferData(uint32_t* width, uint32_t* height, uint32_t* elementSize, 
        void** colorData, void** depthData)
{
    *width = OBDN_WINDOW_WIDTH;
    *height = OBDN_WINDOW_HEIGHT;
    *elementSize = 4;
    *colorData = swapHostBufferColor.hostData;
    *depthData = swapHostBufferDepth.hostData;
}

void r_GetColorDepthExternal(uint32_t* width, uint32_t* height, uint32_t* elementSize, 
        uint64_t* colorOffset, uint64_t* depthOffset)
{
    *width = OBDN_WINDOW_WIDTH;
    *height = OBDN_WINDOW_HEIGHT;
    *elementSize = 4;

    uint32_t frameId = obdn_v_GetCurrentFrameIndex();
    *colorOffset = obdn_v_GetFrame(frameId)->offset;
    *depthOffset = depthAttachment.offset;
}

bool r_GetExtMemoryFd(int* fd, uint64_t* size)
{
    // fast path
    VkMemoryGetFdInfoKHR fdInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
        .memory = obdn_v_GetDeviceMemory(OBDN_V_MEMORY_EXTERNAL_DEVICE_TYPE),
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
    };

    V_ASSERT( vkGetMemoryFdKHR(device, &fdInfo, fd) );

    *size = obdn_v_GetMemorySize(OBDN_V_MEMORY_EXTERNAL_DEVICE_TYPE);

    assert(*size);
    return true;
}

bool r_GetSemaphoreFds(int* obdnFrameDoneFD_0, int* obdnFrameDoneFD_1, int* extTextureReadFD)
{
    VkSemaphoreGetFdInfoKHR fdInfo = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT };

    VkResult r;
    fdInfo.semaphore = extFrameReadSemaphore;
    r = vkGetSemaphoreFdKHR(device, &fdInfo, extTextureReadFD);
    if (r != VK_SUCCESS) {printf("!!! %s ERROR: %d\n", __PRETTY_FUNCTION__, r); assert(0); }
    fdInfo.semaphore = obdn_u_GetSemaphore(0);
    r = vkGetSemaphoreFdKHR(device, &fdInfo, obdnFrameDoneFD_0);
    if (r != VK_SUCCESS) {printf("!!! %s ERROR: %d\n", __PRETTY_FUNCTION__, r); assert(0); }
    fdInfo.semaphore = obdn_u_GetSemaphore(1);
    r = vkGetSemaphoreFdKHR(device, &fdInfo, obdnFrameDoneFD_1);
    if (r != VK_SUCCESS) {printf("!!! %s ERROR: %d\n", __PRETTY_FUNCTION__, r); assert(0); }

    return true;
}

void r_SetExtFastPath(bool isFast)
{
    fastPath = isFast;
}
