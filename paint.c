#include <obsidian/v_image.h>
#include <obsidian/v_memory.h>
#include <obsidian/r_raytrace.h>
#include <obsidian/r_pipeline.h>
#include <obsidian/private.h>
#include <string.h>
#include "layer.h"
#include "obsidian/r_geo.h"
#include "obsidian/v_command.h"
#include "paint.h"
#include "undo.h"
#include "obsidian/t_def.h"
#include "ubo-shared.h"

#define SPVDIR "/home/michaelb/dev/painter/shaders/spv"

enum {
    DESC_SET_PRIM,
    DESC_SET_PAINT,
    DESC_SET_COMP,
    DESC_SET_COUNT
};

enum {
    PIPELINE_COMP_1,
    PIPELINE_COMP_2,
    PIPELINE_COMP_3,
    PIPELINE_COMP_4,
    PIPELINE_COMP_SINGLE,
    PIPELINE_COMP_COUNT
};

typedef Obdn_V_BufferRegion BufferRegion;
typedef Obdn_V_Command Command;
typedef Obdn_V_Image   Image;

static BufferRegion  matrixRegion;
static BufferRegion  brushRegion;

static VkPipeline                 paintPipeline;
static Obdn_R_ShaderBindingTable  shaderBindingTable;

static VkPipeline                 compPipelines[PIPELINE_COMP_COUNT];

static VkDescriptorSetLayout descriptorSetLayouts[DESC_SET_COUNT];
static Obdn_R_Description    description;

static uint32_t graphicsQueueFamilyIndex;
static uint32_t transferQueueFamilyIndex;

static uint32_t textureSize = 0x1000; // 0x1000 = 4096

static Command releaseImageCommand;
static Command transferImageCommand;
static Command acquireImageCommand;

static Command paintCommand;

static Image   imageA; // will use for brush and then as final frambuffer target
static Image   imageB;
static Image   imageC; // primarily background layers
static Image   imageD; // primarily foreground layers

static VkFramebuffer   applyPaintFrameBuffer;
static VkFramebuffer   compositeFrameBuffer;
static VkFramebuffer   backgroundFrameBuffer;
static VkFramebuffer   foregroundFrameBuffer;

static VkRenderPass singleCompositeRenderPass;
static VkRenderPass applyPaintRenderPass;
static VkRenderPass compositeRenderPass;

static const VkFormat textureFormat = VK_FORMAT_R8G8B8A8_UNORM;

static const Obdn_R_Primitive* prim;

static Obdn_S_Scene* renderScene;
static const PaintScene* paintScene;

static VkPipelineLayout pipelineLayout;

static Obdn_R_AccelerationStructure bottomLevelAS;
static Obdn_R_AccelerationStructure topLevelAS;

static L_LayerId curLayerId;

static bool brushActive;
static Vec2 prevBrushPos;
static Vec2 brushPos;


static void initPaintImages(void)
{
    imageA = obdn_v_CreateImageAndSampler(textureSize, textureSize, textureFormat, 
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | 
            VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT,
            VK_SAMPLE_COUNT_1_BIT,
            1,
            VK_FILTER_NEAREST, 
            OBDN_V_MEMORY_DEVICE_TYPE);

    imageB = obdn_v_CreateImageAndSampler(textureSize, textureSize, textureFormat, 
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 
            VK_IMAGE_ASPECT_COLOR_BIT,
            VK_SAMPLE_COUNT_1_BIT,
            1,
            VK_FILTER_LINEAR, 
            OBDN_V_MEMORY_DEVICE_TYPE);

    imageC = obdn_v_CreateImageAndSampler(textureSize, textureSize, textureFormat, 
            VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | 
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 
            VK_IMAGE_ASPECT_COLOR_BIT,
            VK_SAMPLE_COUNT_1_BIT,
            1,
            VK_FILTER_LINEAR, 
            OBDN_V_MEMORY_DEVICE_TYPE);
    
    imageD = obdn_v_CreateImageAndSampler(textureSize, textureSize, textureFormat, 
            VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | 
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 
            VK_IMAGE_ASPECT_COLOR_BIT,
            VK_SAMPLE_COUNT_1_BIT,
            1,
            VK_FILTER_LINEAR, 
            OBDN_V_MEMORY_DEVICE_TYPE);

    obdn_v_TransitionImageLayout(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &imageA);
    obdn_v_TransitionImageLayout(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &imageB);
    obdn_v_TransitionImageLayout(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &imageC);
    obdn_v_TransitionImageLayout(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &imageD);

    obdn_v_ClearColorImage(&imageA);
    obdn_v_ClearColorImage(&imageB);
    obdn_v_ClearColorImage(&imageC);
    obdn_v_ClearColorImage(&imageD);

    obdn_v_TransitionImageLayout(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, &imageA);
    obdn_v_TransitionImageLayout(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, &imageB);
    obdn_v_TransitionImageLayout(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, &imageC);
    obdn_v_TransitionImageLayout(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, &imageD);
}

static void initRenderPasses(void)
{
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

static void initUniformBuffers(void)
{
    matrixRegion = obdn_v_RequestBufferRegion(sizeof(UboMatrices), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            OBDN_V_MEMORY_HOST_GRAPHICS_TYPE);
    UboMatrices* matrices = (UboMatrices*)matrixRegion.hostData;
    matrices->model   = m_Ident_Mat4();
    matrices->view    = m_Ident_Mat4();
    matrices->proj    = m_Ident_Mat4();
    matrices->viewInv = m_Ident_Mat4();
    matrices->projInv = m_Ident_Mat4();

    brushRegion = obdn_v_RequestBufferRegion(sizeof(UboBrush), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            OBDN_V_MEMORY_HOST_GRAPHICS_TYPE);
}

static void initDescSetsAndPipeLayouts(void)
{
    const Obdn_R_DescriptorSetInfo descSets[] = {{
            .bindingCount = 3, 
            .bindings = {{
                // uv buffer
                .descriptorCount = 1,
                .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .stageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR
            },{ // index buffer
                .descriptorCount = 1,
                .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .stageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR
            },{ // tlas
                .descriptorCount = 1,
                .type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
                .stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR
            }},
        },{
            .bindingCount = 3, 
            .bindings = {{
                // matrices
                .descriptorCount = 1,
                .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR
            },{ // brush 
                .descriptorCount = 1,
                .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR
            },{ // paint image
                .descriptorCount = 1,
                .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR
            }},
        },{ // comp 
            .bindingCount = 4,
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
            },{
                .descriptorCount = 1,
                .type = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
            }}
        }
    };

    obdn_r_CreateDescriptorSetLayouts(OBDN_ARRAY_SIZE(descSets), descSets, descriptorSetLayouts);

    obdn_r_CreateDescriptorSets(OBDN_ARRAY_SIZE(descSets), descSets, descriptorSetLayouts, &description);

    VkPushConstantRange pcRange = {
        .stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR,
        .offset     = 0,
        .size       = sizeof(float) * 4
    }; 

    const Obdn_R_PipelineLayoutInfo pipeLayoutInfos[] = {{
        .descriptorSetCount = OBDN_ARRAY_SIZE(descSets), 
        .descriptorSetLayouts = descriptorSetLayouts,
        .pushConstantCount = 1,
        .pushConstantsRanges = &pcRange
    }};

    obdn_r_CreatePipelineLayouts(OBDN_ARRAY_SIZE(pipeLayoutInfos), pipeLayoutInfos, &pipelineLayout);
}

static void updateDescSetPrim(void)
{
    VkWriteDescriptorSetAccelerationStructureKHR asInfo = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
        .accelerationStructureCount = 1,
        .pAccelerationStructures    = &topLevelAS.handle
    };

    VkDescriptorBufferInfo uvBufInfo = {
        .offset = obdn_r_GetAttrOffset(prim, "uv"),
        .range  = obdn_r_GetAttrRange(prim, "uv"),
        .buffer = prim->vertexRegion.buffer,
    };

    VkDescriptorBufferInfo indexBufInfo = {
        .offset = prim->indexRegion.offset,
        .range  = prim->indexRegion.size,
        .buffer = prim->indexRegion.buffer,
    };

    VkWriteDescriptorSet writes[] = {{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstArrayElement = 0,
            .dstSet = description.descriptorSets[DESC_SET_PRIM],
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &uvBufInfo
        },{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstArrayElement = 0,
            .dstSet = description.descriptorSets[DESC_SET_PRIM],
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &indexBufInfo
        },{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstArrayElement = 0,
            .dstSet = description.descriptorSets[DESC_SET_PRIM],
            .dstBinding = 2,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
            .pNext = &asInfo
    }};

    vkUpdateDescriptorSets(device, OBDN_ARRAY_SIZE(writes), writes, 0, NULL);
}

static void updateDescSetPaint(void)
{
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

    VkDescriptorImageInfo imageInfo = {
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .imageView = imageA.view,
        .sampler   = imageA.sampler
    };

    VkWriteDescriptorSet writes[] = {{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstArrayElement = 0,
            .dstSet = description.descriptorSets[DESC_SET_PAINT],
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .pBufferInfo = &uniformInfoMatrices
    },{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstArrayElement = 0,
            .dstSet = description.descriptorSets[DESC_SET_PAINT],
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .pBufferInfo = &uniformInfoBrush
    },{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstArrayElement = 0,
            .dstSet = description.descriptorSets[DESC_SET_PAINT], 
            .dstBinding = 2,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .pImageInfo = &imageInfo
    }};

    vkUpdateDescriptorSets(device, OBDN_ARRAY_SIZE(writes), writes, 0, NULL);
}

static void updateDescSetComp(void)
{
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
            .dstSet = description.descriptorSets[DESC_SET_COMP], 
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
            .pImageInfo = &imageInfoA 
        },{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstArrayElement = 0,
            .dstSet = description.descriptorSets[DESC_SET_COMP], 
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
            .pImageInfo = &imageInfoC
        },{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstArrayElement = 0,
            .dstSet = description.descriptorSets[DESC_SET_COMP], 
            .dstBinding = 2,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
            .pImageInfo = &imageInfoB
        },{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstArrayElement = 0,
            .dstSet = description.descriptorSets[DESC_SET_COMP], 
            .dstBinding = 3,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
            .pImageInfo = &imageInfoD
    }};

    vkUpdateDescriptorSets(device, OBDN_ARRAY_SIZE(writes), writes, 0, NULL);
}

static void initPaintPipelineAndShaderBindingTable(void)
{
    const Obdn_R_RayTracePipelineInfo pipeInfosRT[] = {{
        // ray trace
        .layout = pipelineLayout,
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

    obdn_r_CreateRayTracePipelines(OBDN_ARRAY_SIZE(pipeInfosRT), pipeInfosRT, &paintPipeline, &shaderBindingTable);
}

static void initCompPipelines(const Obdn_R_BlendMode blendMode)
{
    assert(blendMode != OBDN_R_BLEND_MODE_NONE);

    const Obdn_R_GraphicsPipelineInfo pipeInfo1 = {
        .layout  = pipelineLayout,
        .renderPass = applyPaintRenderPass, 
        .subpass = 0,
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .sampleCount = VK_SAMPLE_COUNT_1_BIT,
        .viewportDim = {textureSize, textureSize},
        .blendMode   = blendMode,
        .vertShader = obdn_r_FullscreenTriVertShader(),
        .fragShader = SPVDIR"/comp-frag.spv"
    };

    const Obdn_R_GraphicsPipelineInfo pipeInfo2 = {
        .layout  = pipelineLayout,
        .renderPass = compositeRenderPass, 
        .subpass = 0,
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .sampleCount = VK_SAMPLE_COUNT_1_BIT,
        .viewportDim = {textureSize, textureSize},
        .blendMode   = OBDN_R_BLEND_MODE_OVER_STRAIGHT,
        .vertShader = obdn_r_FullscreenTriVertShader(),
        .fragShader = SPVDIR"/comp2a-frag.spv"
    };

    const Obdn_R_GraphicsPipelineInfo pipeInfo3 = {
        .layout  = pipelineLayout,
        .renderPass = compositeRenderPass, 
        .subpass = 1,
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .sampleCount = VK_SAMPLE_COUNT_1_BIT,
        .viewportDim = {textureSize, textureSize},
        .blendMode   = OBDN_R_BLEND_MODE_OVER_STRAIGHT,
        .vertShader = obdn_r_FullscreenTriVertShader(),
        .fragShader = SPVDIR"/comp3a-frag.spv"
    };

    const Obdn_R_GraphicsPipelineInfo pipeInfo4 = {
        .layout  = pipelineLayout,
        .renderPass = compositeRenderPass, 
        .subpass = 2,
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .sampleCount = VK_SAMPLE_COUNT_1_BIT,
        .viewportDim = {textureSize, textureSize},
        .blendMode   = OBDN_R_BLEND_MODE_OVER_STRAIGHT,
        .vertShader = obdn_r_FullscreenTriVertShader(),
        .fragShader = SPVDIR"/comp4a-frag.spv"
    };

    const Obdn_R_GraphicsPipelineInfo pipeInfoSingle = {
        .layout  = pipelineLayout,
        .renderPass = singleCompositeRenderPass, 
        .subpass = 0,
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .sampleCount = VK_SAMPLE_COUNT_1_BIT,
        .viewportDim = {textureSize, textureSize},
        .blendMode   = OBDN_R_BLEND_MODE_OVER_STRAIGHT,
        .vertShader = obdn_r_FullscreenTriVertShader(),
        .fragShader = SPVDIR"/comp-frag.spv"
    };

    const Obdn_R_GraphicsPipelineInfo infos[] = {
        pipeInfo1, pipeInfo2, pipeInfo3, pipeInfo4, pipeInfoSingle
    };

    assert(OBDN_ARRAY_SIZE(infos) == PIPELINE_COMP_COUNT);

    obdn_r_CreateGraphicsPipelines(OBDN_ARRAY_SIZE(infos), infos, compPipelines);
}

static void destroyCompPipelines(void)
{
    for (int i = PIPELINE_COMP_1; i < PIPELINE_COMP_COUNT; i++)
    {
        vkDestroyPipeline(device, compPipelines[i], NULL);
    }
}

static void initFramebuffers(void)
{
    // applyPaintFrameBuffer 
    {
        const VkImageView attachments[] = {
            imageA.view, imageB.view,
        };

        VkFramebufferCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .layers = 1,
            .height = textureSize,
            .width  = textureSize,
            .renderPass = applyPaintRenderPass,
            .attachmentCount = 2,
            .pAttachments = attachments
        };

        V_ASSERT( vkCreateFramebuffer(device, &info, NULL, &applyPaintFrameBuffer) );
    }

    // compositeFrameBuffer
    {
        const VkImageView attachments[] = {
            imageB.view, imageC.view, imageD.view, imageA.view
        };

        VkFramebufferCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .layers = 1,
            .height = textureSize,
            .width  = textureSize,
            .renderPass = compositeRenderPass,
            .attachmentCount = 4,
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
            .height = textureSize,
            .width  = textureSize,
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
            .height = textureSize,
            .width  = textureSize,
            .renderPass = singleCompositeRenderPass,
            .attachmentCount = 2,
            .pAttachments = attachments
        };

        V_ASSERT( vkCreateFramebuffer(device, &info, NULL, &foregroundFrameBuffer) );
    }
}

static void onLayerChange(L_LayerId newLayerId)
{
    printf("Begin %s\n", __PRETTY_FUNCTION__);

    Obdn_V_Command cmd = obdn_v_CreateCommand(OBDN_V_QUEUE_GRAPHICS_TYPE);

    obdn_v_BeginCommandBuffer(cmd.buffer);

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
            0, 0, NULL, 0, NULL, OBDN_ARRAY_SIZE(barriers), barriers);

    obdn_v_CmdCopyImageToBuffer(cmd.buffer, &imageB, VK_IMAGE_ASPECT_COLOR_BIT, prevLayerBuffer);

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
            0, 0, NULL, 0, NULL, OBDN_ARRAY_SIZE(barriers0), barriers0);

    curLayerId = newLayerId;

    for (int l = 0; l < curLayerId; l++) 
    {
        BufferRegion* layerBuffer = &l_GetLayer(l)->bufferRegion;

        obdn_v_CmdCopyBufferToImage(cmd.buffer, layerBuffer, &imageA);

        VkClearValue clear = {0.0f, 0.903f, 0.009f, 1.0f};

        const VkRenderPassBeginInfo rpass = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .clearValueCount = 1,
            .pClearValues = &clear,
            .renderArea = {{0, 0}, {textureSize, textureSize}},
            .renderPass =  singleCompositeRenderPass,
            .framebuffer = backgroundFrameBuffer,
        };

        vkCmdBeginRenderPass(cmd.buffer, &rpass, VK_SUBPASS_CONTENTS_INLINE);

        vkCmdBindDescriptorSets(
            cmd.buffer, 
            VK_PIPELINE_BIND_POINT_GRAPHICS, 
            pipelineLayout,
            DESC_SET_COMP, 1, &description.descriptorSets[DESC_SET_COMP], 
            0, NULL);

        vkCmdBindPipeline(cmd.buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, compPipelines[PIPELINE_COMP_SINGLE]);

        vkCmdDraw(cmd.buffer, 3, 1, 0, 0);

        vkCmdEndRenderPass(cmd.buffer);
    }

    const int layerCount = l_GetLayerCount();

    for (int l = curLayerId + 1; l < layerCount; l++) 
    {
        BufferRegion* layerBuffer = &l_GetLayer(l)->bufferRegion;

        obdn_v_CmdCopyBufferToImage(cmd.buffer, layerBuffer, &imageA);

        VkClearValue clear = {0.0f, 0.903f, 0.009f, 1.0f};

        const VkRenderPassBeginInfo rpass = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .clearValueCount = 1,
            .pClearValues = &clear,
            .renderArea = {{0, 0}, {textureSize, textureSize}},
            .renderPass =  singleCompositeRenderPass,
            .framebuffer = foregroundFrameBuffer,
        };

        vkCmdBeginRenderPass(cmd.buffer, &rpass, VK_SUBPASS_CONTENTS_INLINE);

        // may not need this because same parameters as last bind?
        vkCmdBindDescriptorSets(
            cmd.buffer, 
            VK_PIPELINE_BIND_POINT_GRAPHICS, 
            pipelineLayout,
            DESC_SET_COMP, 1, &description.descriptorSets[DESC_SET_COMP], 
            0, NULL);

        vkCmdBindPipeline(cmd.buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, compPipelines[PIPELINE_COMP_SINGLE]);

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

    obdn_v_CmdCopyBufferToImage(cmd.buffer, layerBuffer, &imageB);

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
            0, 0, NULL, 0, NULL, OBDN_ARRAY_SIZE(barriers), barriers2);

    obdn_v_EndCommandBuffer(cmd.buffer);

    obdn_v_SubmitAndWait(&cmd, 0);

    obdn_v_DestroyCommand(cmd);

    printf("End %s\n", __PRETTY_FUNCTION__);
}

static void runUndoCommands(const bool toHost, BufferRegion* bufferRegion)
{
    obdn_v_WaitForFence(&acquireImageCommand.fence);

    obdn_v_ResetCommand(&releaseImageCommand);
    obdn_v_ResetCommand(&transferImageCommand);
    obdn_v_ResetCommand(&acquireImageCommand);

    VkCommandBuffer cmdBuf = releaseImageCommand.buffer;

    obdn_v_BeginCommandBuffer(cmdBuf);

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

    obdn_v_EndCommandBuffer(cmdBuf);

    cmdBuf = transferImageCommand.buffer;

    obdn_v_BeginCommandBuffer(cmdBuf);

    vkCmdPipelineBarrier(cmdBuf, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 
            VK_DEPENDENCY_BY_REGION_BIT, 0, NULL, 0, NULL, 1, &imgBarrier);

    if (toHost)
        obdn_v_CmdCopyImageToBuffer(cmdBuf, &imageB, VK_IMAGE_ASPECT_COLOR_BIT, bufferRegion);
    else
        obdn_v_CmdCopyBufferToImage(cmdBuf, bufferRegion, &imageB);

    imgBarrier.srcAccessMask = otherAccessMask;
    imgBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    imgBarrier.oldLayout = otherLayout;
    imgBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgBarrier.srcQueueFamilyIndex = transferQueueFamilyIndex;
    imgBarrier.dstQueueFamilyIndex = graphicsQueueFamilyIndex;

    vkCmdPipelineBarrier(cmdBuf, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 
            VK_DEPENDENCY_BY_REGION_BIT, 0, NULL, 0, NULL, 1, &imgBarrier);

    obdn_v_EndCommandBuffer(cmdBuf);

    cmdBuf = acquireImageCommand.buffer;

    obdn_v_BeginCommandBuffer(cmdBuf);

    vkCmdPipelineBarrier(cmdBuf, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 
            VK_DEPENDENCY_BY_REGION_BIT, 0, NULL, 0, NULL, 1, &imgBarrier);

    obdn_v_EndCommandBuffer(cmdBuf);

    obdn_v_SubmitGraphicsCommand(0, 0, NULL, releaseImageCommand.semaphore, VK_NULL_HANDLE, releaseImageCommand.buffer);
    
    obdn_v_SubmitTransferCommand(0, VK_PIPELINE_STAGE_TRANSFER_BIT, &releaseImageCommand.semaphore, VK_NULL_HANDLE, &transferImageCommand);

    obdn_v_SubmitGraphicsCommand(0, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 
            transferImageCommand.semaphore, 
            acquireImageCommand.semaphore, 
            acquireImageCommand.fence, 
            acquireImageCommand.buffer);
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

static void updateView(void)
{
    UboMatrices* matrices = (UboMatrices*)matrixRegion.hostData;
    matrices->view = renderScene->camera.view;
    matrices->viewInv = renderScene->camera.xform;
}

static void updateProj(void)
{
    UboMatrices* matrices = (UboMatrices*)matrixRegion.hostData;
    matrices->proj = renderScene->camera.proj;
    matrices->projInv = m_Invert4x4(&renderScene->camera.proj);
}

static void updateBrushColor(float r, float g, float b)
{
    UboBrush* brush = (UboBrush*)brushRegion.hostData;
    brush->r = r;
    brush->g = g;
    brush->b = b;
}

static void updateBrush(void)
{
    UboBrush* brush = (UboBrush*)brushRegion.hostData;
    if (paintScene->paint_mode != PAINT_MODE_ERASE)
        updateBrushColor(paintScene->brush_r, paintScene->brush_g, paintScene->brush_b);
    else
        updateBrushColor(1, 1, 1); // must be white for erase to work

    brushActive = paintScene->brush_active;

    prevBrushPos.x = brushPos.x;
    prevBrushPos.y = brushPos.y;
    brushPos.x = paintScene->brush_x;
    brushPos.y = paintScene->brush_y;

    brush->radius = paintScene->brush_radius;
    brush->x = paintScene->brush_x;
    brush->y = paintScene->brush_y;
    brush->opacity = paintScene->brush_opacity;
    brush->anti_falloff = (1.0 - paintScene->brush_falloff) * paintScene->brush_radius;
}

static void updatePaintMode(void)
{
    vkDeviceWaitIdle(device);
    destroyCompPipelines();
    switch (paintScene->paint_mode)
    {
        case PAINT_MODE_OVER:  initCompPipelines(OBDN_R_BLEND_MODE_OVER); break;
        case PAINT_MODE_ERASE: initCompPipelines(OBDN_R_BLEND_MODE_ERASE); break;
    }
}

static void splat(const VkCommandBuffer cmdBuf, const float x, const float y)
{
    vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, paintPipeline);

    vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, 
            pipelineLayout, 0, 2, description.descriptorSets, 0, NULL);

    float pc[4] = {coal_Rand(), coal_Rand(), x, y};

    vkCmdPushConstants(cmdBuf, pipelineLayout, VK_SHADER_STAGE_RAYGEN_BIT_KHR, 0, sizeof(pc), pc);

    vkCmdTraceRaysKHR(cmdBuf, 
            &shaderBindingTable.raygenTable,
            &shaderBindingTable.missTable,
            &shaderBindingTable.hitTable,
            &shaderBindingTable.callableTable,
            2000, 2000, 1);
}

static void applyPaint(const VkCommandBuffer cmdBuf)
{
    VkClearValue clear = {0, 0, 0, 0};

    const VkRenderPassBeginInfo rpass = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .clearValueCount = 1,
        .pClearValues = &clear,
        .renderArea = {{0, 0}, {textureSize, textureSize}},
        .renderPass =  applyPaintRenderPass,
        .framebuffer = applyPaintFrameBuffer 
    };

    vkCmdBeginRenderPass(cmdBuf, &rpass, VK_SUBPASS_CONTENTS_INLINE);

        vkCmdBindDescriptorSets(
            cmdBuf, 
            VK_PIPELINE_BIND_POINT_GRAPHICS, 
            pipelineLayout,
            DESC_SET_COMP, 1, &description.descriptorSets[DESC_SET_COMP], 
            0, NULL);

        vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, compPipelines[PIPELINE_COMP_1]);

        vkCmdDraw(cmdBuf, 3, 1, 0, 0);

    vkCmdEndRenderPass(cmdBuf);
}

static void comp(const VkCommandBuffer cmdBuf)
{
    VkClearValue clear = {0, 0, 0, 0};

    VkClearValue clears[] = {
        clear, clear, clear, clear
    };

    const VkRenderPassBeginInfo rpass = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .clearValueCount = OBDN_ARRAY_SIZE(clears),
        .pClearValues = clears,
        .renderArea = {{0, 0}, {textureSize, textureSize}},
        .renderPass =  compositeRenderPass,
        .framebuffer = compositeFrameBuffer
    };

    vkCmdBeginRenderPass(cmdBuf, &rpass, VK_SUBPASS_CONTENTS_INLINE);

        vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, compPipelines[PIPELINE_COMP_2]);

        vkCmdDraw(cmdBuf, 3, 1, 0, 0);

        vkCmdNextSubpass(cmdBuf, VK_SUBPASS_CONTENTS_INLINE);

        vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, compPipelines[PIPELINE_COMP_3]);

        vkCmdDraw(cmdBuf, 3, 1, 0, 0);

        vkCmdNextSubpass(cmdBuf, VK_SUBPASS_CONTENTS_INLINE);

        vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, compPipelines[PIPELINE_COMP_4]);

        vkCmdDraw(cmdBuf, 3, 1, 0, 0);

    vkCmdEndRenderPass(cmdBuf);
}

static VkSemaphore syncScene()
{
    VkSemaphore semaphore = VK_NULL_HANDLE;
    if (paintScene->dirt || renderScene->dirt)
    {
        if (renderScene->dirt & OBDN_S_CAMERA_VIEW_BIT)
            updateView();
        if (renderScene->dirt & OBDN_S_CAMERA_PROJ_BIT)
            updateProj();
        if (paintScene->dirt & SCENE_BRUSH_BIT)
            updateBrush();
        if (paintScene->dirt & SCENE_UNDO_BIT)
        {
            if (undo())
                semaphore = acquireImageCommand.semaphore;
        }
        if (paintScene->dirt & SCENE_LAYER_CHANGED_BIT)
        {
            onLayerChange(paintScene->layer);
        }
        if (paintScene->dirt & SCENE_LAYER_BACKUP_BIT)
        {
            backupLayer();
            semaphore = acquireImageCommand.semaphore;
        }
        if (paintScene->dirt & SCENE_PAINT_MODE_BIT)
        {
            updatePaintMode();
        }
    }
    return semaphore;
}

static void updateCommands()
{
    VkCommandBuffer cmdBuf = paintCommand.buffer;

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

            splat(cmdBuf, x, y);

            applyPaint(cmdBuf);

            vkCmdClearColorImage(cmdBuf, imageA.handle, VK_IMAGE_LAYOUT_GENERAL, &clearColor, 1, &imageRange);
        }
    }
    else
        applyPaint(cmdBuf);

    comp(cmdBuf);

    V_ASSERT( vkEndCommandBuffer(cmdBuf) );
}

void p_SavePaintImage(void)
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

VkSemaphore p_Paint(VkSemaphore waitSemaphore)
{
    assert(renderScene->primCount == 1);
    obdn_v_WaitForFence(&paintCommand.fence);
    obdn_v_ResetCommand(&paintCommand);
    waitSemaphore = syncScene();
    updateCommands();
    obdn_v_SubmitGraphicsCommand(0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 
            waitSemaphore, paintCommand.semaphore, 
            paintCommand.fence, paintCommand.buffer);
    return paintCommand.semaphore;
}

void p_Init(Obdn_S_Scene* sScene, const PaintScene* pScene, const uint32_t texSize)
{
    assert(sScene->primCount > 0);
    prim  = &sScene->prims[0].rprim;
    paintScene = pScene;
    renderScene = sScene;

    assert(prim->vertexRegion.size);
    obdn_r_BuildBlas(prim, &bottomLevelAS);
    obdn_r_BuildTlas(&bottomLevelAS, &topLevelAS);

    assert(texSize > 0);
    assert(texSize % 256 == 0);
    assert(texSize == IMG_4K || texSize == IMG_8K || texSize == IMG_16K); // for now

    textureSize = texSize;
    curLayerId = 0;
    graphicsQueueFamilyIndex = obdn_v_GetQueueFamilyIndex(OBDN_V_QUEUE_GRAPHICS_TYPE);
    transferQueueFamilyIndex = obdn_v_GetQueueFamilyIndex(OBDN_V_QUEUE_TRANSFER_TYPE);

    paintCommand = obdn_v_CreateCommand(OBDN_V_QUEUE_GRAPHICS_TYPE);

    releaseImageCommand  = obdn_v_CreateCommand(OBDN_V_QUEUE_GRAPHICS_TYPE);
    transferImageCommand = obdn_v_CreateCommand(OBDN_V_QUEUE_TRANSFER_TYPE);
    acquireImageCommand  = obdn_v_CreateCommand(OBDN_V_QUEUE_GRAPHICS_TYPE);

    initPaintImages();

    initRenderPasses();
    initDescSetsAndPipeLayouts();
    initUniformBuffers();
    initPaintPipelineAndShaderBindingTable();
    initCompPipelines(OBDN_R_BLEND_MODE_OVER);

    initFramebuffers();

    assert(imageA.size > 0);

    l_Init(imageA.size); // eventually will move this out
    uint8_t maxUndoStacks, maxUndosPerStack;
    switch (texSize)
    {
        case IMG_4K:  maxUndoStacks = 4; maxUndosPerStack = 8; break;
        case IMG_8K:  maxUndoStacks = 2; maxUndosPerStack = 8; break;
        case IMG_16K: maxUndoStacks = 1; maxUndosPerStack = 8; break;
        default: assert(0);
    }
    u_InitUndo(imageA.size, maxUndoStacks, maxUndosPerStack);
    onLayerChange(0);

    updateDescSetPrim();
    updateDescSetPaint();
    updateDescSetComp();

    renderScene->textures[1].devImage = imageA;
}
