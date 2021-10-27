#include "engine.h"
#include "dtags.h"
#include "layer.h"
#include "private.h"
#include "ubo-shared.h"
#include "undo.h"
#include "stdlib.h"
#include <hell/common.h>
#include <hell/debug.h>
#include <hell/len.h>
#include <hell/locations.h>
#include <hell/minmax.h>
#include <obsidian/command.h>
#include <obsidian/geo.h>
#include <obsidian/image.h>
#include <obsidian/memory.h>
#include <obsidian/pipeline.h>
#include <obsidian/raytrace.h>
#include <obsidian/util.h>
#include <stdio.h>
#include <math.h>
#include <string.h>

#ifdef SPVDIR_PREFIX 
#define SPVDIR SPVDIR_PREFIX "/dali"
#else
#define SPVDIR "dali"
#endif

#ifdef DALI_BRUSH_STROKE_DEBUG_MESSAGES 
#define DPRINT(fmt, ...) hell_DebugPrint("dali_brush", fmt, ##__VA_ARGS__)
#else 
#define DPRINT(fmt, ...) (void)0 
#endif

enum { DESC_SET_PRIM, DESC_SET_PAINT, DESC_SET_COMP, DESC_SET_COUNT };

enum {
    PIPELINE_COMP_1,
    PIPELINE_COMP_2,
    PIPELINE_COMP_3,
    PIPELINE_COMP_4,
    PIPELINE_COMP_SINGLE,
    PIPELINE_COMP_COUNT
};

#define PRIM_DIRTY_BITS (DALI_PRIM_ADDED_BIT | DALI_PRIM_CHANGED_BIT)

typedef Obdn_BufferRegion BufferRegion;

typedef Obdn_Command Command;
typedef Obdn_Image   Image;

typedef enum EngineState {
    DEAD, // will cause paint to return if engine struct is 0'd out
    READY,
    NEEDS_TO_CREATE_IMAGES
} EngineState;

typedef struct Dali_Engine {
    BufferRegion matrixRegion;
    BufferRegion brushRegion;

    VkPipeline                paintPipeline;
    Obdn_R_ShaderBindingTable shaderBindingTable;

    VkPipeline compPipelines[PIPELINE_COMP_COUNT];

    VkDescriptorSetLayout descriptorSetLayouts[DESC_SET_COUNT];
    Obdn_R_Description    description;

    uint32_t graphicsQueueFamilyIndex;
    uint32_t transferQueueFamilyIndex;

    uint32_t textureSize; // = 0x1000; // 0x1000 = 4096

    Command cmdReleaseImageTransferSource;
    Command cmdTranferImage;
    Command cmdAcquireImageTranferSource;
    Command paintCommand;

    Image imageA; // will use for brush and then as final frambuffer target
    Image imageB;
    Image imageC; // primarily background layers
    Image imageD; // primarily foreground layers
    
    // default alpha is created once and 
    // it is shared by all brushes across 
    Image defaultBrushAlpha; 

    VkFramebuffer applyPaintFrameBuffer;
    VkFramebuffer compositeFrameBuffer;
    VkFramebuffer backgroundFrameBuffer;
    VkFramebuffer foregroundFrameBuffer;

    VkRenderPass singleCompositeRenderPass;
    VkRenderPass applyPaintRenderPass;
    VkRenderPass compositeRenderPass;

    Dali_Format textureFormat; // = VK_FORMAT_R8G8B8A8_UNORM;

    // Obdn_S_Scene* renderScene;
    // const Dali_Brush* brush;

    VkPipelineLayout pipelineLayout;

    Obdn_R_AccelerationStructure bottomLevelAS;
    Obdn_R_AccelerationStructure topLevelAS;

    Dali_LayerId curLayerId;

    Obdn_PrimitiveHandle activePrim;

    EngineState          state;

    uint32_t             dirt;
    
    bool                 brushActive;
    bool                 brushWasActive;
    float                strokeLength;
    float                brushSpacing;
    uint32_t             rayWidth; // sqrt of ray count (rays per splat)
    Vec2                 brushPos;
    Vec2                 prevBrushPos;
    float                brushAngle;
    float                brushAngleVariation;
    Obdn_Memory*         memory;
    const Obdn_Instance* instance;
    VkDevice             device;
} Dali_Engine;

typedef Dali_Engine Engine;

#define DTAG PAINT_DEBUG_TAG_PAINT

static void
initPaintImages(Dali_Engine* engine)
{
    VkFormat textureFormat;
    switch (engine->textureFormat)
    {
        case DALI_FORMAT_R8G8B8A8_UNORM: textureFormat = VK_FORMAT_B8G8R8A8_UNORM; break; 
        case DALI_FORMAT_R32_SFLOAT:     textureFormat = VK_FORMAT_R32_SFLOAT; break;
    }
    engine->imageA = obdn_CreateImageAndSampler(
        engine->memory, engine->textureSize, engine->textureSize,
        textureFormat,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | 
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_STORAGE_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT, VK_SAMPLE_COUNT_1_BIT, 1, VK_FILTER_NEAREST,
        OBDN_MEMORY_DEVICE_TYPE);

    engine->imageB = obdn_CreateImageAndSampler(
        engine->memory, engine->textureSize, engine->textureSize,
        textureFormat,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT, VK_SAMPLE_COUNT_1_BIT, 1, VK_FILTER_LINEAR,
        OBDN_MEMORY_DEVICE_TYPE);

    engine->imageC = obdn_CreateImageAndSampler(
        engine->memory, engine->textureSize, engine->textureSize,
        textureFormat,
        VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT, VK_SAMPLE_COUNT_1_BIT, 1, VK_FILTER_LINEAR,
        OBDN_MEMORY_DEVICE_TYPE);

    engine->imageD = obdn_CreateImageAndSampler(
        engine->memory, engine->textureSize, engine->textureSize,
        textureFormat,
        VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT, VK_SAMPLE_COUNT_1_BIT, 1, VK_FILTER_LINEAR,
        OBDN_MEMORY_DEVICE_TYPE);

    obdn_TransitionImageLayout(VK_IMAGE_LAYOUT_UNDEFINED,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               &engine->imageA);
    obdn_TransitionImageLayout(VK_IMAGE_LAYOUT_UNDEFINED,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               &engine->imageB);
    obdn_TransitionImageLayout(VK_IMAGE_LAYOUT_UNDEFINED,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               &engine->imageC);
    obdn_TransitionImageLayout(VK_IMAGE_LAYOUT_UNDEFINED,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               &engine->imageD);

    obdn_v_ClearColorImage(&engine->imageA);
    obdn_v_ClearColorImage(&engine->imageB);
    obdn_v_ClearColorImage(&engine->imageC);
    obdn_v_ClearColorImage(&engine->imageD);

    obdn_TransitionImageLayout(VK_IMAGE_LAYOUT_UNDEFINED,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                               &engine->imageA);
    obdn_TransitionImageLayout(VK_IMAGE_LAYOUT_UNDEFINED,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                               &engine->imageB);
    obdn_TransitionImageLayout(VK_IMAGE_LAYOUT_UNDEFINED,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                               &engine->imageC);
    obdn_TransitionImageLayout(VK_IMAGE_LAYOUT_UNDEFINED,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                               &engine->imageD);
}

static void
initRenderPasses(Engine* engine)
{
    // apply paint renderpass
    VkFormat textureFormat;
    switch (engine->textureFormat)
    {
        case DALI_FORMAT_R8G8B8A8_UNORM: textureFormat = VK_FORMAT_B8G8R8A8_UNORM; break; 
        case DALI_FORMAT_R32_SFLOAT:     textureFormat = VK_FORMAT_R32_SFLOAT; break;
    }

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
            .layout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

        const VkAttachmentReference referenceB1 = {
            .attachment = 1,
            .layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

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

        VkSubpassDependency dependencies[] = {dependency1, dependency2};

        VkAttachmentDescription attachments[] = {attachmentA, attachmentB};

        VkRenderPassCreateInfo ci = {
            .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
            .subpassCount    = 1,
            .pSubpasses      = &subpass1,
            .attachmentCount = LEN(attachments),
            .pAttachments    = attachments,
            .dependencyCount = LEN(dependencies),
            .pDependencies   = dependencies,
        };

        V_ASSERT(vkCreateRenderPass(engine->device, &ci, NULL,
                                    &engine->applyPaintRenderPass));
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
            .layout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

        const VkAttachmentReference referenceC2 = {
            .attachment = 1,
            .layout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

        const VkAttachmentReference referenceD2 = {
            .attachment = 2,
            .layout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

        const VkAttachmentReference referenceA2 = {
            .attachment = 3,
            .layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

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
            subpass1,
            subpass2,
            subpass3,
        };

        VkSubpassDependency dependencies[] = {
            dependency1,
            dependency2,
            dependency3,
            dependency4,
        };

        VkAttachmentDescription attachments[] = {attachmentB, attachmentC,
                                                 attachmentD, attachmentA2};

        VkRenderPassCreateInfo ci = {
            .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
            .subpassCount    = LEN(subpasses),
            .pSubpasses      = subpasses,
            .attachmentCount = LEN(attachments),
            .pAttachments    = attachments,
            .dependencyCount = LEN(dependencies),
            .pDependencies   = dependencies,
        };

        V_ASSERT(vkCreateRenderPass(engine->device, &ci, NULL,
                                    &engine->compositeRenderPass));
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
            .layout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

        const VkAttachmentReference refDst = {
            .attachment = 1,
            .layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

        const VkSubpassDescription subpass = {
            .pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .colorAttachmentCount    = 1,
            .pColorAttachments       = &refDst,
            .pDepthStencilAttachment = NULL,
            .inputAttachmentCount    = 1,
            .pInputAttachments       = &refSrc,
            .preserveAttachmentCount = 0,
        };

        const VkSubpassDependency dependencies[] = {
            {
                .srcSubpass    = VK_SUBPASS_EXTERNAL,
                .dstSubpass    = 0,
                .srcStageMask  = VK_PIPELINE_STAGE_TRANSFER_BIT,
                .srcAccessMask = 0,
                .dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            },
            {
                .srcSubpass    = 0,
                .dstSubpass    = VK_SUBPASS_EXTERNAL,
                .srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                .dstStageMask  = VK_PIPELINE_STAGE_TRANSFER_BIT,
                .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            }};

        const VkAttachmentDescription attachments[] = {srcAttachment,
                                                       dstAttachment};

        VkRenderPassCreateInfo ci = {
            .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
            .subpassCount    = 1,
            .pSubpasses      = &subpass,
            .attachmentCount = LEN(attachments),
            .pAttachments    = attachments,
            .dependencyCount = LEN(dependencies),
            .pDependencies   = dependencies,
        };

        V_ASSERT(vkCreateRenderPass(engine->device, &ci, NULL,
                                    &engine->singleCompositeRenderPass));
    }
}

static void
initUniformBuffers(Engine* engine)
{
    engine->matrixRegion = obdn_RequestBufferRegion(
        engine->memory, sizeof(UboMatrices), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        OBDN_MEMORY_HOST_GRAPHICS_TYPE);
    UboMatrices* matrices = (UboMatrices*)engine->matrixRegion.hostData;
    matrices->model       = coal_Ident_Mat4();
    matrices->view        = coal_Ident_Mat4();
    matrices->proj        = coal_Ident_Mat4();
    matrices->viewInv     = coal_Ident_Mat4();
    matrices->projInv     = coal_Ident_Mat4();

    engine->brushRegion = obdn_RequestBufferRegion(
        engine->memory, sizeof(UboBrush), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        OBDN_MEMORY_HOST_GRAPHICS_TYPE);
}

static void
initDescSetsAndPipeLayouts(Engine* engine)
{
    Obdn_DescriptorBinding bindingsA[] = {
        {// uv buffer
         .descriptorCount = 1,
         .type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .stageFlags      = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR},
        {// index buffer
         .descriptorCount = 1,
         .type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .stageFlags      = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR},
        {// tlas
         .descriptorCount = 1,
         .type            = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
         .stageFlags      = VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                       VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR}};

    Obdn_DescriptorBinding bindingsB[] = {
        {// matrices
         .descriptorCount = 1,
         .type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
         .stageFlags      = VK_SHADER_STAGE_RAYGEN_BIT_KHR},
        {// brush
         .descriptorCount = 1,
         .type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
         .stageFlags      = VK_SHADER_STAGE_RAYGEN_BIT_KHR},
        {// paint image
         .descriptorCount = 1,
         .type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         .stageFlags      = VK_SHADER_STAGE_RAYGEN_BIT_KHR},
        {// alpha image
         .descriptorCount = 1,
         .type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .stageFlags      = VK_SHADER_STAGE_RAYGEN_BIT_KHR}
    };

    Obdn_DescriptorBinding bindingsC[] = {
        {
            .descriptorCount = 1,
            .type            = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
            .stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
        {
            .descriptorCount = 1,
            .type            = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
            .stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
        {
            .descriptorCount = 1,
            .type            = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
            .stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
        {
            .descriptorCount = 1,
            .type            = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
            .stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT,
        }};

    const Obdn_DescriptorSetInfo descSets[] = {
        {
            .bindingCount = LEN(bindingsA),
            .bindings     = bindingsA,
        },
        {
            .bindingCount = LEN(bindingsB),
            .bindings     = bindingsB,
        },
        {// comp
         .bindingCount = LEN(bindingsC),
         .bindings     = bindingsC}};

    obdn_CreateDescriptorSetLayouts(engine->device, LEN(descSets), descSets,
                                    engine->descriptorSetLayouts);

    obdn_CreateDescriptorSets(engine->device, LEN(descSets), descSets,
                              engine->descriptorSetLayouts,
                              &engine->description);

    // seedx, seedy, x, y, angle
    VkPushConstantRange pcRange = {.stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR,
                                   .offset     = 0,
                                   .size       = sizeof(float) * 5};

    const Obdn_PipelineLayoutInfo pipeLayoutInfos[] = {
        {.descriptorSetCount   = LEN(descSets),
         .descriptorSetLayouts = engine->descriptorSetLayouts,
         .pushConstantCount    = 1,
         .pushConstantsRanges  = &pcRange}};

    obdn_CreatePipelineLayouts(engine->device, LEN(pipeLayoutInfos),
                               pipeLayoutInfos, &engine->pipelineLayout);
}

static void 
createDefaultBrushAlpha(Engine* engine)
{
    Obdn_Image image = obdn_CreateImageAndSampler(engine->memory, 32, 32, VK_FORMAT_R32_SFLOAT,
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT, VK_SAMPLE_COUNT_1_BIT, 1, VK_FILTER_LINEAR, OBDN_MEMORY_DEVICE_TYPE);

    Obdn_Command cmd = obdn_CreateCommand(engine->instance, OBDN_V_QUEUE_GRAPHICS_TYPE);
    
    VkImageSubresourceRange ssRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, 
            .baseArrayLayer = 0,
            .baseMipLevel = 0,
            .levelCount = 1,
            .layerCount = 1
    };

    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .image = image.handle,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .subresourceRange = ssRange
    };

    VkClearColorValue color = {
        .float32 = {1., 1., 1., 1.}
    };

    obdn_BeginCommandBuffer(cmd.buffer);
    vkCmdPipelineBarrier(cmd.buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_DEPENDENCY_BY_REGION_BIT, 0, NULL, 0, NULL, 1,
                         &barrier);
    vkCmdClearColorImage(cmd.buffer, image.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &ssRange);

    barrier.srcAccessMask = barrier.dstAccessMask;
    barrier.dstAccessMask = 0;
    barrier.oldLayout = barrier.newLayout;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;

    vkCmdPipelineBarrier(cmd.buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         VK_DEPENDENCY_BY_REGION_BIT, 0, NULL, 0, NULL, 1,
                         &barrier);

    obdn_EndCommandBuffer(cmd.buffer);

    obdn_SubmitAndWait(&cmd, 0);

    engine->defaultBrushAlpha = image;
}

static void
updateDescSetPrim(Engine* engine, const Obdn_Scene* scene)
{
    VkWriteDescriptorSetAccelerationStructureKHR asInfo = {
        .sType =
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
        .accelerationStructureCount = 1,
        .pAccelerationStructures    = &engine->topLevelAS.handle};

    Obdn_Primitive* prim = obdn_GetPrimitive(scene, engine->activePrim.id);

    VkDescriptorBufferInfo uvBufInfo = {
        .offset = obdn_GetAttrOffset(prim->geo, "uv"),
        .range  = obdn_GetAttrRange(prim->geo, "uv"),
        .buffer = prim->geo->vertexRegion.buffer,
    };

    VkDescriptorBufferInfo indexBufInfo = {
        .offset = prim->geo->indexRegion.offset,
        .range  = prim->geo->indexRegion.size,
        .buffer = prim->geo->indexRegion.buffer,
    };

    VkWriteDescriptorSet writes[] = {
        {.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstArrayElement = 0,
         .dstSet          = engine->description.descriptorSets[DESC_SET_PRIM],
         .dstBinding      = 0,
         .descriptorCount = 1,
         .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo     = &uvBufInfo},
        {.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstArrayElement = 0,
         .dstSet          = engine->description.descriptorSets[DESC_SET_PRIM],
         .dstBinding      = 1,
         .descriptorCount = 1,
         .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo     = &indexBufInfo},
        {.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstArrayElement = 0,
         .dstSet          = engine->description.descriptorSets[DESC_SET_PRIM],
         .dstBinding      = 2,
         .descriptorCount = 1,
         .descriptorType  = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
         .pNext           = &asInfo}};

    vkUpdateDescriptorSets(engine->device, LEN(writes), writes, 0, NULL);
}

static void 
updateDescriptorsAlphaImage(Engine* engine, const Obdn_Image* image)
{
    VkDescriptorImageInfo imageInfo = {.imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                       .imageView   = image->view,
                                       .sampler     = image->sampler};
    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .dstSet          = engine->description.descriptorSets[DESC_SET_PAINT],
        .dstBinding      = 3,
        .descriptorCount = 1,
        .pImageInfo      = &imageInfo
    };

    vkUpdateDescriptorSets(engine->device, 1, &write, 0, NULL);
}

static void 
updateDescriptorsPaintUBOS(Engine* engine)
{
    VkDescriptorBufferInfo uniformInfoMatrices = {
        .range  = engine->matrixRegion.size,
        .offset = engine->matrixRegion.offset,
        .buffer = engine->matrixRegion.buffer,
    };

    VkDescriptorBufferInfo uniformInfoBrush = {
        .range  = engine->brushRegion.size,
        .offset = engine->brushRegion.offset,
        .buffer = engine->brushRegion.buffer,
    };

    VkWriteDescriptorSet writes[] = {
        {.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstArrayElement = 0,
         .dstSet          = engine->description.descriptorSets[DESC_SET_PAINT],
         .dstBinding      = 0,
         .descriptorCount = 1,
         .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
         .pBufferInfo     = &uniformInfoMatrices},
        {.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstArrayElement = 0,
         .dstSet          = engine->description.descriptorSets[DESC_SET_PAINT],
         .dstBinding      = 1,
         .descriptorCount = 1,
         .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
         .pBufferInfo     = &uniformInfoBrush}};

    vkUpdateDescriptorSets(engine->device, LEN(writes), writes, 0, NULL);
}

static void 
updateDescriptorsPaintImage(Engine* engine)
{
    VkDescriptorImageInfo imageInfo = {.imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                       .imageView   = engine->imageA.view,
                                       .sampler     = engine->imageA.sampler};
    VkWriteDescriptorSet write =  {
         .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstArrayElement = 0,
         .dstSet          = engine->description.descriptorSets[DESC_SET_PAINT],
         .dstBinding      = 2,
         .descriptorCount = 1,
         .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         .pImageInfo      = &imageInfo};

    vkUpdateDescriptorSets(engine->device, 1, &write, 0, NULL);
}

static void
updateAllPaintDescriptors(Engine* engine)
{
    updateDescriptorsPaintUBOS(engine);
    updateDescriptorsPaintImage(engine);

    updateDescriptorsAlphaImage(engine, &engine->defaultBrushAlpha);
}

static void
updateDescSetComp(Engine* engine)
{
    VkDescriptorImageInfo imageInfoA = {
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .imageView   = engine->imageA.view,
        .sampler     = engine->imageA.sampler};

    VkDescriptorImageInfo imageInfoB = {
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .imageView   = engine->imageB.view,
        .sampler     = engine->imageB.sampler};

    VkDescriptorImageInfo imageInfoC = {
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .imageView   = engine->imageC.view,
        .sampler     = engine->imageC.sampler};

    VkDescriptorImageInfo imageInfoD = {
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .imageView   = engine->imageD.view,
        .sampler     = engine->imageD.sampler};

    VkWriteDescriptorSet writes[] = {
        {.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstArrayElement = 0,
         .dstSet          = engine->description.descriptorSets[DESC_SET_COMP],
         .dstBinding      = 0,
         .descriptorCount = 1,
         .descriptorType  = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
         .pImageInfo      = &imageInfoA},
        {.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstArrayElement = 0,
         .dstSet          = engine->description.descriptorSets[DESC_SET_COMP],
         .dstBinding      = 1,
         .descriptorCount = 1,
         .descriptorType  = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
         .pImageInfo      = &imageInfoC},
        {.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstArrayElement = 0,
         .dstSet          = engine->description.descriptorSets[DESC_SET_COMP],
         .dstBinding      = 2,
         .descriptorCount = 1,
         .descriptorType  = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
         .pImageInfo      = &imageInfoB},
        {.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstArrayElement = 0,
         .dstSet          = engine->description.descriptorSets[DESC_SET_COMP],
         .dstBinding      = 3,
         .descriptorCount = 1,
         .descriptorType  = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
         .pImageInfo      = &imageInfoD}};

    vkUpdateDescriptorSets(engine->device, LEN(writes), writes, 0, NULL);
}

static void
initPaintPipelineAndShaderBindingTable(Engine* engine)
{
    char raygenShader[32];
    switch (engine->textureFormat)
    {
        case DALI_FORMAT_R8G8B8A8_UNORM: strcpy(raygenShader, SPVDIR "/paint.rgen.spv"); break;
        case DALI_FORMAT_R32_SFLOAT:     strcpy(raygenShader, SPVDIR "/paint32R.rgen.spv"); break;
    }
    const Obdn_RayTracePipelineInfo pipeInfosRT[] = {
        {// ray trace
         .layout      = engine->pipelineLayout,
         .raygenCount = 1,
         .raygenShaders =
             (char*[]){
                 raygenShader
             },
         .missCount = 1,
         .missShaders =
             (char*[]){
                 SPVDIR "/paint.rmiss.spv",
             },
         .chitCount   = 1,
         .chitShaders = (char*[]){SPVDIR "/paint.rchit.spv"}}};

    obdn_CreateRayTracePipelines(
        engine->device, engine->memory, LEN(pipeInfosRT), pipeInfosRT,
        &engine->paintPipeline, &engine->shaderBindingTable);
}

static void
initCompPipelines(Engine* engine, Dali_PaintMode paintMode)
{
    Obdn_BlendMode splatBlendMode, compBlendMode;
    switch (engine->textureFormat)
    {
    case DALI_FORMAT_R8G8B8A8_UNORM: 
        compBlendMode  = OBDN_BLEND_MODE_OVER_NO_PREMUL;
        switch (paintMode)
        {
        case DALI_PAINT_MODE_OVER: splatBlendMode = OBDN_BLEND_MODE_OVER; break;
        case DALI_PAINT_MODE_ERASE: splatBlendMode = OBDN_BLEND_MODE_ERASE; break;
        } break;
    case DALI_FORMAT_R32_SFLOAT:
        compBlendMode  = OBDN_BLEND_MODE_OVER_NO_PREMUL_MONOCHROME;
        switch (paintMode)
        {
        case DALI_PAINT_MODE_OVER: splatBlendMode = OBDN_BLEND_MODE_OVER_MONOCHROME; break;
        case DALI_PAINT_MODE_ERASE: splatBlendMode = OBDN_BLEND_MODE_ERASE_MONOCHROME; break;
        } break;
    default:
        hell_Print("Error. Unsupported textureFormat\n");
        exit(1);
    }

    const Obdn_GraphicsPipelineInfo pipeInfo1 = {
        .layout            = engine->pipelineLayout,
        .renderPass        = engine->applyPaintRenderPass,
        .subpass           = 0,
        .frontFace         = VK_FRONT_FACE_CLOCKWISE,
        .sampleCount       = VK_SAMPLE_COUNT_1_BIT,
        .primitiveTopology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .viewportDim       = {engine->textureSize, engine->textureSize},
        .blendMode         = splatBlendMode,
        .vertShader        = OBDN_FULL_SCREEN_VERT_SPV,
        .fragShader        = SPVDIR "/comp.frag.spv"};

    const Obdn_GraphicsPipelineInfo pipeInfo2 = {
        .layout            = engine->pipelineLayout,
        .renderPass        = engine->compositeRenderPass,
        .subpass           = 0,
        .frontFace         = VK_FRONT_FACE_CLOCKWISE,
        .sampleCount       = VK_SAMPLE_COUNT_1_BIT,
        .primitiveTopology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .viewportDim       = {engine->textureSize, engine->textureSize},
        .blendMode         = compBlendMode,
        .vertShader        = OBDN_FULL_SCREEN_VERT_SPV,
        .fragShader        = SPVDIR "/comp2a.frag.spv"};

    const Obdn_GraphicsPipelineInfo pipeInfo3 = {
        .layout            = engine->pipelineLayout,
        .renderPass        = engine->compositeRenderPass,
        .subpass           = 1,
        .frontFace         = VK_FRONT_FACE_CLOCKWISE,
        .sampleCount       = VK_SAMPLE_COUNT_1_BIT,
        .primitiveTopology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .viewportDim       = {engine->textureSize, engine->textureSize},
        .blendMode         = compBlendMode,
        .vertShader        = OBDN_FULL_SCREEN_VERT_SPV,
        .fragShader        = SPVDIR "/comp3a.frag.spv"};

    const Obdn_GraphicsPipelineInfo pipeInfo4 = {
        .layout            = engine->pipelineLayout,
        .renderPass        = engine->compositeRenderPass,
        .subpass           = 2,
        .frontFace         = VK_FRONT_FACE_CLOCKWISE,
        .sampleCount       = VK_SAMPLE_COUNT_1_BIT,
        .primitiveTopology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .viewportDim       = {engine->textureSize, engine->textureSize},
        .blendMode         = compBlendMode,
        .vertShader        = OBDN_FULL_SCREEN_VERT_SPV,
        .fragShader        = SPVDIR "/comp4a.frag.spv"};

    const Obdn_GraphicsPipelineInfo pipeInfoSingle = {
        .layout            = engine->pipelineLayout,
        .renderPass        = engine->singleCompositeRenderPass,
        .subpass           = 0,
        .frontFace         = VK_FRONT_FACE_CLOCKWISE,
        .sampleCount       = VK_SAMPLE_COUNT_1_BIT,
        .primitiveTopology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .viewportDim       = {engine->textureSize, engine->textureSize},
        .blendMode         = compBlendMode,
        .vertShader        = OBDN_FULL_SCREEN_VERT_SPV,
        .fragShader        = SPVDIR "/comp.frag.spv"};

    const Obdn_GraphicsPipelineInfo infos[] = {pipeInfo1, pipeInfo2, pipeInfo3,
                                               pipeInfo4, pipeInfoSingle};

    assert(LEN(infos) == PIPELINE_COMP_COUNT);

    obdn_CreateGraphicsPipelines(engine->device, LEN(infos), infos,
                                 engine->compPipelines);
}

static void
destroyCompPipelines(Engine* engine)
{
    for (int i = PIPELINE_COMP_1; i < PIPELINE_COMP_COUNT; i++)
    {
        vkDestroyPipeline(engine->device, engine->compPipelines[i], NULL);
    }
}

static void
initFramebuffers(Engine* engine)
{
    // applyPaintFrameBuffer
    {
        const VkImageView attachments[] = {
            engine->imageA.view,
            engine->imageB.view,
        };

        VkFramebufferCreateInfo info = {
            .sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .layers          = 1,
            .height          = engine->textureSize,
            .width           = engine->textureSize,
            .renderPass      = engine->applyPaintRenderPass,
            .attachmentCount = 2,
            .pAttachments    = attachments};

        V_ASSERT(vkCreateFramebuffer(engine->device, &info, NULL,
                                     &engine->applyPaintFrameBuffer));
    }

    // compositeFrameBuffer
    {
        const VkImageView attachments[] = {
            engine->imageB.view, engine->imageC.view, engine->imageD.view,
            engine->imageA.view};

        VkFramebufferCreateInfo info = {
            .sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .layers          = 1,
            .height          = engine->textureSize,
            .width           = engine->textureSize,
            .renderPass      = engine->compositeRenderPass,
            .attachmentCount = 4,
            .pAttachments    = attachments};

        V_ASSERT(vkCreateFramebuffer(engine->device, &info, NULL,
                                     &engine->compositeFrameBuffer));
    }

    // backgroundFrameBuffer
    {
        const VkImageView attachments[] = {
            engine->imageA.view,
            engine->imageC.view,
        };

        VkFramebufferCreateInfo info = {
            .sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .layers          = 1,
            .height          = engine->textureSize,
            .width           = engine->textureSize,
            .renderPass      = engine->singleCompositeRenderPass,
            .attachmentCount = 2,
            .pAttachments    = attachments};

        V_ASSERT(vkCreateFramebuffer(engine->device, &info, NULL,
                                     &engine->backgroundFrameBuffer));
    }

    // foregroundFrameBuffer
    {
        const VkImageView attachments[] = {
            engine->imageA.view,
            engine->imageD.view,
        };

        VkFramebufferCreateInfo info = {
            .sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .layers          = 1,
            .height          = engine->textureSize,
            .width           = engine->textureSize,
            .renderPass      = engine->singleCompositeRenderPass,
            .attachmentCount = 2,
            .pAttachments    = attachments};

        V_ASSERT(vkCreateFramebuffer(engine->device, &info, NULL,
                                     &engine->foregroundFrameBuffer));
    }
}

static void
onLayerChange(Engine* engine, Dali_LayerStack* stack, Dali_LayerId newLayerId)
{
    hell_DebugPrint(PAINT_DEBUG_TAG_PAINT, "Begin\n");

    Obdn_Command cmd =
        obdn_CreateCommand(engine->instance, OBDN_V_QUEUE_GRAPHICS_TYPE);

    obdn_BeginCommandBuffer(cmd.buffer);

    BufferRegion* prevLayerBuffer =
        &dali_GetLayer(stack, engine->curLayerId)->bufferRegion;

    VkImageSubresourceRange subResRange = {
        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseArrayLayer = 0,
        .baseMipLevel   = 0,
        .levelCount     = 1,
        .layerCount     = 1,
    };

    VkClearColorValue clearColor = {
        .float32[0] = 0,
        .float32[1] = 0,
        .float32[2] = 0,
        .float32[3] = 0,
    };

    VkImageMemoryBarrier barriers[] = {
        {.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .image            = engine->imageA.handle,
         .oldLayout        = engine->imageA.layout,
         .newLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         .subresourceRange = subResRange,
         .srcAccessMask    = 0,
         .dstAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT},
        {.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .image            = engine->imageB.handle,
         .oldLayout        = engine->imageB.layout,
         .newLayout        = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
         .subresourceRange = subResRange,
         .srcAccessMask    = 0,
         .dstAccessMask    = VK_ACCESS_TRANSFER_READ_BIT},
        {.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .image            = engine->imageC.handle,
         .oldLayout        = engine->imageC.layout,
         .newLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         .subresourceRange = subResRange,
         .srcAccessMask    = 0,
         .dstAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT},
        {.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .image            = engine->imageD.handle,
         .oldLayout        = engine->imageD.layout,
         .newLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         .subresourceRange = subResRange,
         .srcAccessMask    = 0,
         .dstAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT}};

    vkCmdPipelineBarrier(cmd.buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                         LEN(barriers), barriers);

    obdn_CmdCopyImageToBuffer(cmd.buffer, 0, &engine->imageB, prevLayerBuffer);

    vkCmdClearColorImage(cmd.buffer, engine->imageC.handle,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1,
                         &subResRange);
    vkCmdClearColorImage(cmd.buffer, engine->imageD.handle,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1,
                         &subResRange);

    VkImageMemoryBarrier barriers0[] = {
        {.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .image            = engine->imageC.handle,
         .oldLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         .newLayout        = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
         .subresourceRange = subResRange,
         .srcAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT,
         .dstAccessMask    = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT},
        {.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .image            = engine->imageD.handle,
         .oldLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         .newLayout        = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
         .subresourceRange = subResRange,
         .srcAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT,
         .dstAccessMask    = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT}};

    vkCmdPipelineBarrier(cmd.buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                         NULL, 0, NULL, LEN(barriers0), barriers0);

    engine->curLayerId = newLayerId;

    for (int l = 0; l < engine->curLayerId; l++)
    {
        BufferRegion* layerBuffer = &dali_GetLayer(stack, l)->bufferRegion;

        obdn_CmdCopyBufferToImage(cmd.buffer, 0, layerBuffer, &engine->imageA);

        VkClearValue clear = {0.0f, 0.903f, 0.009f, 1.0f};

        const VkRenderPassBeginInfo rpass = {
            .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .clearValueCount = 1,
            .pClearValues    = &clear,
            .renderArea  = {{0, 0}, {engine->textureSize, engine->textureSize}},
            .renderPass  = engine->singleCompositeRenderPass,
            .framebuffer = engine->backgroundFrameBuffer,
        };

        vkCmdBeginRenderPass(cmd.buffer, &rpass, VK_SUBPASS_CONTENTS_INLINE);

        vkCmdBindDescriptorSets(
            cmd.buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, engine->pipelineLayout,
            DESC_SET_COMP, 1,
            &engine->description.descriptorSets[DESC_SET_COMP], 0, NULL);

        vkCmdBindPipeline(cmd.buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          engine->compPipelines[PIPELINE_COMP_SINGLE]);

        vkCmdDraw(cmd.buffer, 3, 1, 0, 0);

        vkCmdEndRenderPass(cmd.buffer);
    }

    const int layerCount = dali_GetLayerCount(stack);

    for (int l = engine->curLayerId + 1; l < layerCount; l++)
    {
        BufferRegion* layerBuffer = &dali_GetLayer(stack, l)->bufferRegion;

        obdn_CmdCopyBufferToImage(cmd.buffer, 0, layerBuffer, &engine->imageA);

        VkClearValue clear = {0.0f, 0.903f, 0.009f, 1.0f};

        const VkRenderPassBeginInfo rpass = {
            .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .clearValueCount = 1,
            .pClearValues    = &clear,
            .renderArea  = {{0, 0}, {engine->textureSize, engine->textureSize}},
            .renderPass  = engine->singleCompositeRenderPass,
            .framebuffer = engine->foregroundFrameBuffer,
        };

        vkCmdBeginRenderPass(cmd.buffer, &rpass, VK_SUBPASS_CONTENTS_INLINE);

        // may not need this because same parameters as last bind?
        vkCmdBindDescriptorSets(
            cmd.buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, engine->pipelineLayout,
            DESC_SET_COMP, 1,
            &engine->description.descriptorSets[DESC_SET_COMP], 0, NULL);

        vkCmdBindPipeline(cmd.buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          engine->compPipelines[PIPELINE_COMP_SINGLE]);

        vkCmdDraw(cmd.buffer, 3, 1, 0, 0);

        vkCmdEndRenderPass(cmd.buffer);
    }

    VkImageMemoryBarrier barrier1 = {
        .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .image            = engine->imageB.handle,
        .oldLayout        = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .newLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .subresourceRange = subResRange,
        .srcAccessMask    = VK_ACCESS_TRANSFER_READ_BIT,
        .dstAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT};

    vkCmdPipelineBarrier(cmd.buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
                         &barrier1);

    BufferRegion* layerBuffer =
        &dali_GetLayer(stack, engine->curLayerId)->bufferRegion;

    obdn_CmdCopyBufferToImage(cmd.buffer, 0, layerBuffer, &engine->imageB);

    VkImageMemoryBarrier barriers2[] = {
        {.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .image            = engine->imageA.handle,
         .oldLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         .newLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
         .subresourceRange = subResRange,
         .srcAccessMask    = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT,
         .dstAccessMask    = 0},
        {.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .image            = engine->imageB.handle,
         .oldLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         .newLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
         .subresourceRange = subResRange,
         .srcAccessMask    = VK_ACCESS_MEMORY_READ_BIT,
         .dstAccessMask    = 0},
        {.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .image            = engine->imageC.handle,
         .oldLayout        = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
         .newLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
         .subresourceRange = subResRange,
         .srcAccessMask    = VK_ACCESS_MEMORY_WRITE_BIT,
         .dstAccessMask    = 0},
        {.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .image            = engine->imageD.handle,
         .oldLayout        = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
         .newLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
         .subresourceRange = subResRange,
         .srcAccessMask    = VK_ACCESS_MEMORY_WRITE_BIT,
         .dstAccessMask    = 0}};

    vkCmdPipelineBarrier(cmd.buffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0,
                         NULL, LEN(barriers), barriers2);

    obdn_EndCommandBuffer(cmd.buffer);

    obdn_SubmitAndWait(&cmd, 0);

    obdn_DestroyCommand(cmd);

    hell_DebugPrint(PAINT_DEBUG_TAG_PAINT, "End\n");
}

static void
runUndoCommands(Engine* engine, const bool toHost /*vs fromHost*/, BufferRegion* bufferRegion)
{
    obdn_WaitForFence(engine->device, &engine->cmdAcquireImageTranferSource.fence);

    obdn_ResetCommand(&engine->cmdReleaseImageTransferSource);
    obdn_ResetCommand(&engine->cmdTranferImage);
    obdn_ResetCommand(&engine->cmdAcquireImageTranferSource);

    VkCommandBuffer cmdBuf = engine->cmdReleaseImageTransferSource.buffer;

    obdn_BeginCommandBuffer(cmdBuf);

    const VkImageSubresourceRange range = {.aspectMask =
                                               VK_IMAGE_ASPECT_COLOR_BIT,
                                           .baseMipLevel   = 0,
                                           .levelCount     = 1,
                                           .baseArrayLayer = 0,
                                           .layerCount     = 1};

    const VkImageLayout otherLayout =
        toHost ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
               : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    const VkAccessFlags otherAccessMask =
        toHost ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_TRANSFER_WRITE_BIT;

    VkImageMemoryBarrier imgBarrier = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext               = NULL,
        .srcAccessMask       = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT,
        .dstAccessMask       = 0, /* ignored for this batter */
        .oldLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .newLayout           = otherLayout,
        .srcQueueFamilyIndex = engine->graphicsQueueFamilyIndex,
        .dstQueueFamilyIndex = engine->transferQueueFamilyIndex,
        .image               = engine->imageB.handle,
        .subresourceRange    = range};

    vkCmdPipelineBarrier(cmdBuf, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_DEPENDENCY_BY_REGION_BIT, 0, NULL, 0, NULL, 1,
                         &imgBarrier);

    obdn_EndCommandBuffer(cmdBuf);

    cmdBuf = engine->cmdTranferImage.buffer;

    obdn_BeginCommandBuffer(cmdBuf);

// if is where the first validation message occurs
    imgBarrier.srcAccessMask = 0;
    imgBarrier.dstAccessMask = otherAccessMask;

    vkCmdPipelineBarrier(cmdBuf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_DEPENDENCY_BY_REGION_BIT, 0, NULL, 0, NULL, 1,
                         &imgBarrier);

    if (toHost)
        obdn_CmdCopyImageToBuffer(cmdBuf, 0, &engine->imageB, bufferRegion);
    else
        obdn_CmdCopyBufferToImage(cmdBuf, 0, bufferRegion, &engine->imageB);

    imgBarrier.srcAccessMask       = otherAccessMask;
    imgBarrier.dstAccessMask       = 0; //again, not used on this half of the ownership tranfer but validation layers complain
    imgBarrier.oldLayout           = otherLayout;
    imgBarrier.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgBarrier.srcQueueFamilyIndex = engine->transferQueueFamilyIndex;
    imgBarrier.dstQueueFamilyIndex = engine->graphicsQueueFamilyIndex;

    vkCmdPipelineBarrier(cmdBuf, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         VK_DEPENDENCY_BY_REGION_BIT, 0, NULL, 0, NULL, 1,
                         &imgBarrier);

    obdn_EndCommandBuffer(cmdBuf);

    cmdBuf = engine->cmdAcquireImageTranferSource.buffer;

    obdn_BeginCommandBuffer(cmdBuf);

    imgBarrier.srcAccessMask = 0; // will be ignored 
    imgBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmdBuf, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_DEPENDENCY_BY_REGION_BIT, 0, NULL, 0, NULL, 1,
                         &imgBarrier);

    obdn_EndCommandBuffer(cmdBuf);

    obdn_SubmitGraphicsCommand(
        engine->instance, 0, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, NULL, 1,
        &engine->cmdReleaseImageTransferSource.semaphore, VK_NULL_HANDLE,
        engine->cmdReleaseImageTransferSource.buffer);

    obdn_SubmitTransferCommand(engine->instance, 0,
                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                               &engine->cmdReleaseImageTransferSource.semaphore,
                               VK_NULL_HANDLE, &engine->cmdTranferImage);

    obdn_SubmitGraphicsCommand(
        engine->instance, 0, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 1,
        &engine->cmdTranferImage.semaphore, 1,
        &engine->cmdAcquireImageTranferSource.semaphore,
        engine->cmdAcquireImageTranferSource.fence, engine->cmdAcquireImageTranferSource.buffer);
}

static void
backupLayer(Engine* engine, Dali_UndoManager* undo)
{
    runUndoCommands(engine, true, dali_GetNextUndoBuffer(undo));
    hell_DebugPrint(DTAG, "layer backed up\n");
}

static bool
undo(Engine* engine, Dali_UndoManager* undo)
{
    hell_DebugPrint(DTAG, "undo\n");
    BufferRegion* buf = dali_GetLastUndoBuffer(undo);
    if (!buf)
        return false; // nothing to undo
    runUndoCommands(engine, false, buf);
    return true;
}

static void
updateView(Engine* engine, const Obdn_Scene* scene)
{
    UboMatrices* matrices = (UboMatrices*)engine->matrixRegion.hostData;
    matrices->view        = obdn_GetCameraView(scene);
    matrices->viewInv     = coal_Invert4x4(matrices->view); //TODO: replace with cam.xform
}

static void
updateProj(Engine* engine, const Obdn_Scene* scene)
{
    UboMatrices* matrices = (UboMatrices*)engine->matrixRegion.hostData;
    matrices->proj        = obdn_GetCameraProjection(scene);
    matrices->projInv     = coal_Invert4x4(matrices->proj);
}

static void
updateBrushColor(Engine* engine, float r, float g, float b)
{
    UboBrush* brush = (UboBrush*)engine->brushRegion.hostData;
    brush->r        = r;
    brush->g        = g;
    brush->b        = b;
}

static void
syncBrush(Engine* engine, const Dali_Brush* b)
{
    UboBrush* brush = (UboBrush*)engine->brushRegion.hostData;

    if (b->dirt & BRUSH_PAINT_MODE_BIT)
    {
        vkDeviceWaitIdle(engine->device);
        destroyCompPipelines(engine);
        initCompPipelines(engine, b->mode);
    }

    if (b->dirt & BRUSH_GENERAL_BIT)
    {
        if (b->mode != PAINT_MODE_ERASE)
            updateBrushColor(engine, b->r, b->g, b->b);
        else
            updateBrushColor(engine, 1, 1, 1); // must be white for erase to work
        engine->brushActive = b->active;

        engine->prevBrushPos.x = engine->brushPos.x;
        engine->prevBrushPos.y = engine->brushPos.y;
        engine->brushPos.x     = b->x;
        engine->brushPos.y     = b->y;
        engine->brushSpacing   = b->spacing;
        engine->brushAngle     = b->angle;
        engine->brushAngleVariation = b->angleVariation;

        brush->radius       = b->radius;
        brush->x            = b->x;
        brush->y            = b->y;
        brush->opacity      = b->opacity;
        brush->anti_falloff = (1.0 - b->falloff) * b->radius;
    }

    if (b->dirt & BRUSH_ALPHA_BIT)
    {
        if (b->alphaImg)
        {
            updateDescriptorsAlphaImage(engine, b->alphaImg);
        }
        else 
        {
            updateDescriptorsAlphaImage(engine, &engine->defaultBrushAlpha);
        }
    }
}

static void
updatePrim(Engine* engine, const Obdn_Scene* scene)
{
    assert(engine->activePrim.id != 0);
    Obdn_Primitive* prim = obdn_GetPrimitive(scene, engine->activePrim.id);
    if (prim->dirt & OBDN_PRIM_REMOVED_BIT)
    {
        obdn_DestroyAccelerationStruct(engine->device, &engine->bottomLevelAS);
        obdn_DestroyAccelerationStruct(engine->device, &engine->topLevelAS);
        engine->activePrim = NULL_PRIM;
        return;
    }
    assert(prim->geo);
    if (prim->dirt & OBDN_PRIM_ADDED_BIT || engine->dirt & DALI_PRIM_ADDED_BIT)
    {
        Coal_Mat4 xform = COAL_MAT4_IDENT;
        obdn_BuildBlas(engine->memory, prim->geo, &engine->bottomLevelAS);
        obdn_BuildTlas(engine->memory, 1, &engine->bottomLevelAS, &xform,
                       &engine->topLevelAS);

        updateDescSetPrim(engine, scene);
        return;
    }
    if (prim->dirt & OBDN_PRIM_TOPOLOGY_CHANGED_BIT || engine->dirt & DALI_PRIM_CHANGED_BIT)
    {
        obdn_DestroyAccelerationStruct(engine->device, &engine->bottomLevelAS);
        obdn_DestroyAccelerationStruct(engine->device, &engine->topLevelAS);
        Coal_Mat4 xform = COAL_MAT4_IDENT;
        obdn_BuildBlas(engine->memory, prim->geo, &engine->bottomLevelAS);
        obdn_BuildTlas(engine->memory, 1, &engine->bottomLevelAS, &xform,
                       &engine->topLevelAS);

        updateDescSetPrim(engine, scene);
        return;
    }
}

static void
splat(Engine* engine, const VkCommandBuffer cmdBuf, const float x,
      const float y, float angle, uint32_t rayWidth)
{
    vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                      engine->paintPipeline);

    vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                            engine->pipelineLayout, 0, 2,
                            engine->description.descriptorSets, 0, NULL);

    float pc[5] = {coal_Rand(), coal_Rand(), x, y, angle};

    vkCmdPushConstants(cmdBuf, engine->pipelineLayout,
                       VK_SHADER_STAGE_RAYGEN_BIT_KHR, 0, sizeof(pc), pc);

    vkCmdTraceRaysKHR(cmdBuf, &engine->shaderBindingTable.raygenTable,
                      &engine->shaderBindingTable.missTable,
                      &engine->shaderBindingTable.hitTable,
                      &engine->shaderBindingTable.callableTable, rayWidth, rayWidth, 1);
}

static void
applyPaint(Engine* engine, const VkCommandBuffer cmdBuf)
{
    VkClearValue clear = {0, 0, 0, 0};

    const VkRenderPassBeginInfo rpass = {
        .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .clearValueCount = 1,
        .pClearValues    = &clear,
        .renderArea      = {{0, 0}, {engine->textureSize, engine->textureSize}},
        .renderPass      = engine->applyPaintRenderPass,
        .framebuffer     = engine->applyPaintFrameBuffer};

    vkCmdBeginRenderPass(cmdBuf, &rpass, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            engine->pipelineLayout, DESC_SET_COMP, 1,
                            &engine->description.descriptorSets[DESC_SET_COMP],
                            0, NULL);

    vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      engine->compPipelines[PIPELINE_COMP_1]);

    vkCmdDraw(cmdBuf, 3, 1, 0, 0);

    vkCmdEndRenderPass(cmdBuf);
}

static void
comp(Engine* engine, const VkCommandBuffer cmdBuf)
{
    VkClearValue clear = {0, 0, 0, 0};

    VkClearValue clears[] = {clear, clear, clear, clear};

    const VkRenderPassBeginInfo rpass = {
        .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .clearValueCount = LEN(clears),
        .pClearValues    = clears,
        .renderArea      = {{0, 0}, {engine->textureSize, engine->textureSize}},
        .renderPass      = engine->compositeRenderPass,
        .framebuffer     = engine->compositeFrameBuffer};

    vkCmdBeginRenderPass(cmdBuf, &rpass, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      engine->compPipelines[PIPELINE_COMP_2]);

    vkCmdDraw(cmdBuf, 3, 1, 0, 0);

    vkCmdNextSubpass(cmdBuf, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      engine->compPipelines[PIPELINE_COMP_3]);

    vkCmdDraw(cmdBuf, 3, 1, 0, 0);

    vkCmdNextSubpass(cmdBuf, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      engine->compPipelines[PIPELINE_COMP_4]);

    vkCmdDraw(cmdBuf, 3, 1, 0, 0);

    vkCmdEndRenderPass(cmdBuf);
}

static VkSemaphore
sync(Engine* engine, const Obdn_Scene* scene, Dali_LayerStack* stack,
     const Dali_Brush* brush, Dali_UndoManager* u)
{
    VkSemaphore                semaphore = VK_NULL_HANDLE;
    const Obdn_SceneDirtyFlags sceneDirt = obdn_GetSceneDirt(scene);
    if (engine->dirt & DALI_ENGINE_JUST_CREATED_BIT)
    {
        updateView(engine, scene);
        updateProj(engine, scene);
        syncBrush(engine, brush);
    }
    if (brush->dirt || sceneDirt || stack->dirt || u->dirt || engine->dirt)
    {
        if (sceneDirt & OBDN_SCENE_CAMERA_VIEW_BIT)
            updateView(engine, scene);
        if (sceneDirt & OBDN_SCENE_CAMERA_PROJ_BIT)
            updateProj(engine, scene);
        if (brush->dirt)
            syncBrush(engine, brush);
        if (sceneDirt & OBDN_SCENE_PRIMS_BIT || engine->dirt & PRIM_DIRTY_BITS)
            updatePrim(engine, scene);
        if (u->dirt & UNDO_BIT)
        {
            if (undo(engine, u))
                semaphore = engine->cmdAcquireImageTranferSource.semaphore;
        }
        if (stack->dirt & LAYER_CHANGED_BIT)
        {
            onLayerChange(engine, stack,
                          stack->activeLayer); // only one that needs the stack
        }
        if (stack->dirt & LAYER_BACKUP_BIT)
        {
            backupLayer(engine, u);
            semaphore = engine->cmdAcquireImageTranferSource.semaphore;
        }
    }
    engine->dirt = 0;
    return semaphore;
}

static void
updateCommands(Engine* engine, VkCommandBuffer cmdBuf)
{
    VkClearColorValue clearColor = {
        .float32[0] = 0,
        .float32[1] = 0,
        .float32[2] = 0,
        .float32[3] = 0,
    };

    VkImageSubresourceRange imageRange = {
        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseArrayLayer = 0,
        .baseMipLevel   = 0,
        .levelCount     = 1,
        .layerCount     = 1,
    };

    VkImageMemoryBarrier imgBarrier0 = {
        .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .image            = engine->imageA.handle,
        .oldLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .newLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .subresourceRange = imageRange,
        .srcAccessMask    = 0,
        .dstAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT,
    };

    vkCmdPipelineBarrier(cmdBuf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
                         &imgBarrier0);

    vkCmdClearColorImage(cmdBuf, engine->imageA.handle,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1,
                         &imageRange);

    VkImageMemoryBarrier imgBarrier1 = {
        .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .image            = engine->imageA.handle,
        .oldLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout        = VK_IMAGE_LAYOUT_GENERAL,
        .subresourceRange = imageRange,
        .srcAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask    = VK_ACCESS_SHADER_WRITE_BIT};

    vkCmdPipelineBarrier(cmdBuf, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0, 0,
                         NULL, 0, NULL, 1, &imgBarrier1);

    if (engine->brushActive)
    {
        if (!engine->brushWasActive)
        {
            engine->brushWasActive = true;
            engine->strokeLength = 0.0;
            vkCmdClearColorImage(cmdBuf, engine->imageA.handle,
                                 VK_IMAGE_LAYOUT_GENERAL, &clearColor, 1,
                                 &imageRange);

            splat(engine, cmdBuf, engine->brushPos.x, engine->brushPos.y, engine->brushAngle, engine->rayWidth);
        }
        else 
        {
            const float unit = engine->brushSpacing;
            const float brushDist = coal_Distance(engine->brushPos, engine->prevBrushPos);
            const float remainder = fmodf(engine->strokeLength, unit);
            const float totalNewLength = remainder + brushDist;
            engine->strokeLength += brushDist;
            const int splatCount = (int)(MIN(totalNewLength / unit, 30));
            DPRINT("strokeLength %f brushDist %f remainder %f "
                       "totalNewLength %f Splat count: %d\n",
                       engine->strokeLength, brushDist, remainder,
                       totalNewLength, splatCount);
            Coal_Vec2 splatVector = {
                engine->brushPos.x - engine->prevBrushPos.x, 
                engine->brushPos.y - engine->prevBrushPos.y};
            float t = unit - remainder;
            const float stepSize = 1.0 / splatCount; 
            for (int i = 0; i < splatCount; i++)
            {
                float xstep = t * splatVector.x;
                float ystep = t * splatVector.y;
                float x     = engine->prevBrushPos.x + xstep;
                float y     = engine->prevBrushPos.y + ystep;
                t += stepSize;

                vkCmdClearColorImage(cmdBuf, engine->imageA.handle,
                                     VK_IMAGE_LAYOUT_GENERAL, &clearColor, 1,
                                     &imageRange);

                float var = M_PI * engine->brushAngleVariation;
                float angle = engine->brushAngle;
                splat(engine, cmdBuf, x, y, coal_RandRange(angle - var, angle + var), engine->rayWidth);

                applyPaint(engine, cmdBuf);
            }
        }
    }
    else 
    {
        engine->brushWasActive = false;
    }
    applyPaint(engine, cmdBuf);

    comp(engine, cmdBuf);
}

static void
printTextureDim(Hell_Grimoire* grim, void* enginePtr)
{
    Engine* engine = (Engine*)enginePtr;
    hell_Print("%dx%d\n", engine->textureSize, engine->textureSize);
}

// TODO: see if we can do this by pass a layerstack instead of the engine
void
dali_SavePaintImage(Dali_Engine* engine)
{
    hell_Print("Please enter a file name with extension.\n");
    char strbuf[32];
    if (fgets(strbuf, 32, stdin) == NULL)
    {
        hell_Print("Failed to load image at path. Possibly path exceeds limit "
                   "of 32 characters\n");
        return;
    }
    uint8_t len = strlen(strbuf);
    if (len < 5)
    {
        hell_Print("Filename too small. Must include extension.\n");
        return;
    }
    if (strbuf[len - 1] == '\n')
        strbuf[--len] = '\0';
    const char* ext = strbuf + len - 3;
    hell_Print("%s", ext);
    Obdn_V_ImageFileType fileType;
    if (strncmp(ext, "png", 3) == 0)
        fileType = OBDN_V_IMAGE_FILE_TYPE_PNG;
    else if (strncmp(ext, "jpg", 3) == 0)
        fileType = OBDN_V_IMAGE_FILE_TYPE_JPG;
    else
    {
        hell_Print("Bad extension.\n");
        return;
    }
    obdn_SaveImage(engine->memory, &engine->imageA, fileType, strbuf);
}

static void 
savePaintCmd(Hell_Grimoire* grim, void* pengine)
{
    Dali_Engine* engine = pengine;
    const char* strbuf = hell_GetArg(grim, 1);
    uint8_t len = strlen(strbuf);
    if (len < 5)
    {
        hell_Print("Filename too small. Must include extension.\n");
        return;
    }
    const char* ext = strbuf + len - 3;
    Obdn_V_ImageFileType fileType;
    if (strncmp(ext, "png", 3) == 0)
        fileType = OBDN_V_IMAGE_FILE_TYPE_PNG;
    else if (strncmp(ext, "jpg", 3) == 0)
        fileType = OBDN_V_IMAGE_FILE_TYPE_JPG;
    else
    {
        hell_Print("Bad extension.\n");
        return;
    }
    vkDeviceWaitIdle(engine->device);
    obdn_SaveImage(engine->memory, &engine->imageA, fileType, strbuf);
}

static void 
rayWidthCmd(Hell_Grimoire* grim, void* pengine)
{
    Dali_Engine* engine = pengine;
    int rayWidth = atoi(hell_GetArg(grim, 1));
    if (rayWidth < 1 || rayWidth > 10000)
    {
        hell_Print("Bad value");
        return;
    }
    engine->rayWidth = rayWidth;
}

void
reclaimCmd(Hell_Grimoire* grim, void* pengine)
{
    Engine* engine = pengine;
    dali_EngineCreateImagesAndDependents(engine);
}

VkSemaphore 
dali_Paint(Dali_Engine* engine, const Obdn_Scene* scene,
           const Dali_Brush* brush, Dali_LayerStack* stack,
           Dali_UndoManager* um, VkCommandBuffer cmdbuf)
{
    if (engine->state != READY)
    {
        return VK_NULL_HANDLE;
    }
    VkSemaphore waitSemaphore = sync(engine, scene, stack, brush, um);
    if (engine->activePrim.id == 0) return waitSemaphore;
    updateCommands(engine, cmdbuf);
    return waitSemaphore;
}

void
dali_CreateEngine(const Obdn_Instance* instance, Obdn_Memory* memory,
                          const uint32_t texSize, Dali_Format textureFormat,
                          Hell_Grimoire* grimoire, Engine* engine)
{
    hell_Print("DALI Engine: starting initialization...\n");
    memset(engine, 0, sizeof(Engine));
    engine->instance    = instance;
    engine->memory      = memory;
    engine->device      = obdn_GetDevice(instance);
    engine->textureSize = texSize;
    engine->textureFormat = textureFormat; // TODO: should probably be passed in...

    assert(texSize > 0);
    assert(texSize % 256 == 0);

    engine->curLayerId = 0;
    engine->graphicsQueueFamilyIndex =
        obdn_GetQueueFamilyIndex(instance, OBDN_V_QUEUE_GRAPHICS_TYPE);
    engine->transferQueueFamilyIndex =
        obdn_GetQueueFamilyIndex(instance, OBDN_V_QUEUE_TRANSFER_TYPE);

    engine->paintCommand =
        obdn_CreateCommand(instance, OBDN_V_QUEUE_GRAPHICS_TYPE);

    engine->cmdReleaseImageTransferSource =
        obdn_CreateCommand(instance, OBDN_V_QUEUE_GRAPHICS_TYPE);
    engine->cmdTranferImage =
        obdn_CreateCommand(instance, OBDN_V_QUEUE_TRANSFER_TYPE);
    engine->cmdAcquireImageTranferSource =
        obdn_CreateCommand(instance, OBDN_V_QUEUE_GRAPHICS_TYPE);

    initPaintImages(engine);

    initRenderPasses(engine);
    initDescSetsAndPipeLayouts(engine);
    initUniformBuffers(engine);
    initPaintPipelineAndShaderBindingTable(engine);
    initCompPipelines(engine, DALI_PAINT_MODE_OVER);

    initFramebuffers(engine);

    assert(engine->imageA.size > 0);

    createDefaultBrushAlpha(engine);

    updateAllPaintDescriptors(engine);
    updateDescSetComp(engine);

    engine->rayWidth = 512;
    engine->state = READY;
    engine->dirt |= DALI_ENGINE_JUST_CREATED_BIT;

    if (grimoire)
    {
        hell_AddCommand(grimoire, "texsize", printTextureDim, engine);
        hell_AddCommand(grimoire, "savepaint", savePaintCmd, engine);
        hell_AddCommand(grimoire, "raywidth", rayWidthCmd, engine);
    }

    hell_Print("PAINT: initialized.\n");
}

void
dali_DestroyEngine(Engine* engine, Obdn_Scene* scene)
{
    vkDeviceWaitIdle(engine->device);
    obdn_FreeBufferRegion(&engine->matrixRegion);
    obdn_FreeBufferRegion(&engine->brushRegion);
    vkDestroyPipeline(engine->device, engine->paintPipeline, NULL);
    vkDestroyPipelineLayout(engine->device, engine->pipelineLayout, NULL);
    obdn_DestroyShaderBindingTable(&engine->shaderBindingTable);
    for (int i = 0; i < PIPELINE_COMP_COUNT; i++)
    {
        vkDestroyPipeline(engine->device, engine->compPipelines[i], NULL);
    }
    for (int i = 0; i < DESC_SET_COUNT; i++)
    {
        vkDestroyDescriptorSetLayout(engine->device,
                                     engine->descriptorSetLayouts[i], NULL);
    }
    obdn_DestroyDescription(engine->device, &engine->description);
    obdn_DestroyCommand(engine->cmdReleaseImageTransferSource);
    obdn_DestroyCommand(engine->cmdTranferImage);
    obdn_DestroyCommand(engine->cmdAcquireImageTranferSource);
    obdn_DestroyCommand(engine->paintCommand);

    if (!(engine->state & NEEDS_TO_CREATE_IMAGES))
        dali_EngineDestroyImagesAndDependents(engine);
    obdn_FreeImage(&engine->defaultBrushAlpha);

    vkDestroyRenderPass(engine->device, engine->singleCompositeRenderPass,
                        NULL);
    vkDestroyRenderPass(engine->device, engine->applyPaintRenderPass, NULL);
    vkDestroyRenderPass(engine->device, engine->compositeRenderPass, NULL);
    obdn_DestroyAccelerationStruct(engine->device, &engine->bottomLevelAS);
    obdn_DestroyAccelerationStruct(engine->device, &engine->topLevelAS);
    memset(engine, 0, sizeof(Engine));
}

Dali_Engine*
dali_AllocEngine(void)
{
    return hell_Malloc(sizeof(Dali_Engine));
}

void 
dali_SetActivePrim(Engine* engine, Obdn_PrimitiveHandle prim, Dali_EngineDirt mask)
{
    engine->activePrim = prim;
    engine->dirt |= mask;
}

Obdn_PrimitiveHandle 
dali_GetActivePrim(Dali_Engine* engine)
{
    return engine->activePrim;
}

void 
dali_EngineDestroyImagesAndDependents(Dali_Engine* engine)
{
    vkDeviceWaitIdle(engine->device);
    obdn_FreeImage(&engine->imageA);
    obdn_FreeImage(&engine->imageB);
    obdn_FreeImage(&engine->imageC);
    obdn_FreeImage(&engine->imageD);
    vkDestroyFramebuffer(engine->device, engine->applyPaintFrameBuffer, NULL);
    vkDestroyFramebuffer(engine->device, engine->compositeFrameBuffer, NULL);
    vkDestroyFramebuffer(engine->device, engine->backgroundFrameBuffer, NULL);
    vkDestroyFramebuffer(engine->device, engine->foregroundFrameBuffer, NULL);
    engine->state = NEEDS_TO_CREATE_IMAGES;
}

void
dali_EngineCreateImagesAndDependents(Dali_Engine* engine)
{
    initPaintImages(engine);
    initFramebuffers(engine);
    updateDescriptorsPaintImage(engine);
    updateDescSetComp(engine);
    engine->state = READY;
}

Obdn_Image* 
dali_GetTextureImage(Dali_Engine* e)
{
    return &e->imageA;
}
