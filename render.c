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
#include <obsidian/r_render.h>
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

#include <pthread.h>

#define SPVDIR "/home/michaelb/dev/painter/shaders/spv"

typedef struct {
    float x;
    float y;
    float radius;
    float r;
    float g;
    float b;
    int   mode;
} UboBrush;

typedef struct {
    Mat4 model;
    Mat4 view;
    Mat4 proj;
    Mat4 viewInv;
    Mat4 projInv;
} UboMatrices;

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
    PIPELINE_RAY_TRACE,
    PIPELINE_SELECT,
    RT_PIPELINE_COUNT
};

enum {
    SBT_RAY_TRACE,
    SBT_SELECT,
};

typedef Obdn_V_BufferRegion BufferRegion;

static BufferRegion  matrixRegion;
static BufferRegion  brushRegion;
static BufferRegion  selectionRegion;

typedef struct {
    float x;
    float y;
} PaintJitterSeed;

static Obdn_R_Primitive     renderPrim;

static VkPipelineLayout           pipelineLayouts[OBDN_MAX_PIPELINES];
static VkPipeline                 graphicsPipelines[G_PIPELINE_COUNT];
static VkPipeline                 raytracePipelines[RT_PIPELINE_COUNT];
static Obdn_R_ShaderBindingTable  shaderBindingTables[RT_PIPELINE_COUNT];

static VkDescriptorSetLayout descriptorSetLayouts[OBDN_MAX_DESCRIPTOR_SETS];
static Obdn_R_Description   description;

static Obdn_V_Image   depthAttachment;

static uint32_t graphicsQueueFamilyIndex;
static uint32_t transferQueueFamilyIndex;

static uint32_t textureSize = 0x1000; // 0x1000 = 4096

static Command releaseImageCommand;
static Command transferImageCommand;
static Command acquireImageCommand;

static Command renderCommands[OBDN_FRAME_COUNT];

static Image   imageA; // will use for brush and then as final frambuffer target
static Image   imageB;
static Image   imageC; // primarily background layers
static Image   imageD; // primarily foreground layers

static VkFramebuffer   compositeFrameBuffer;
static VkFramebuffer   backgroundFrameBuffer;
static VkFramebuffer   foregroundFrameBuffer;
static VkFramebuffer   swapchainFrameBuffers[OBDN_FRAME_COUNT];
static VkFramebuffer   postFrameBuffers[OBDN_FRAME_COUNT];

static const VkFormat depthFormat   = VK_FORMAT_D32_SFLOAT;
static const VkFormat textureFormat = VK_FORMAT_R8G8B8A8_UNORM;

static VkRenderPass singleCompositeRenderPass;
static VkRenderPass compositeRenderPass;
static VkRenderPass swapchainRenderPass;
static VkRenderPass postRenderPass;

static Obdn_R_AccelerationStructure bottomLevelAS;
static Obdn_R_AccelerationStructure topLevelAS;

static L_LayerId curLayerId;

static const Scene* scene;

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
static void paint(const VkCommandBuffer cmdBuf);
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

static void initPaintImages(void)
{
    imageA = obdn_v_CreateImageAndSampler(textureSize, textureSize, textureFormat, 
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | 
            VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT,
            VK_SAMPLE_COUNT_1_BIT,
            1,
            VK_FILTER_LINEAR, 
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
    obdn_r_CreateRenderPass_ColorDepth(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, 
            VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
            obdn_r_GetSwapFormat(), depthFormat, &swapchainRenderPass);

    obdn_r_CreateRenderPass_Color(
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ATTACHMENT_LOAD_OP_LOAD, obdn_r_GetSwapFormat(), &postRenderPass);

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

        VkSubpassDescription subpass2 = {
            .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .colorAttachmentCount = 1,
            .pColorAttachments    = &referenceA2,
            .pDepthStencilAttachment = NULL,
            .inputAttachmentCount = 1,
            .pInputAttachments = &referenceC2,
            .preserveAttachmentCount = 0,
        };

        VkSubpassDescription subpass3 = {
            .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .colorAttachmentCount = 1,
            .pColorAttachments    = &referenceA2,
            .pDepthStencilAttachment = NULL,
            .inputAttachmentCount = 1,
            .pInputAttachments = &referenceB2,
            .preserveAttachmentCount = 0,
        };

        VkSubpassDescription subpass4 = {
            .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .colorAttachmentCount = 1,
            .pColorAttachments    = &referenceA2,
            .pDepthStencilAttachment = NULL,
            .inputAttachmentCount = 1,
            .pInputAttachments = &referenceD2,
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
            .dstSubpass = 2,
            .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT,
        };

        const VkSubpassDependency dependency4 = {
            .srcSubpass = 2,
            .dstSubpass = 3,
            .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT,
        };

        const VkSubpassDependency dependency5 = {
            .srcSubpass = 3,
            .dstSubpass = VK_SUBPASS_EXTERNAL,
            .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        };

        VkSubpassDescription subpasses[] = {
            subpass1, subpass2, subpass3, subpass4
        };

        VkSubpassDependency dependencies[] = {
            dependency1, dependency2, dependency3, dependency4, dependency5
        };

        VkAttachmentDescription attachments[] = {
            attachmentA, attachmentB, attachmentC, attachmentD, attachmentA2
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
                .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR
            },{ // attrib buffer
                .descriptorCount = 1,
                .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .stageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR
            },{ // index buffer
                .descriptorCount = 1,
                .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .stageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR
            },{ // paint image
                .descriptorCount = 1,
                .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT
            }}
        },{ // ray trace
            .bindingCount = 4,
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
            },{ // uv buffer
                .descriptorCount = 1,
                .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .stageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR
            }}
        },{ // post
            .bindingCount = 1,
            .bindings = {{
                .descriptorCount = 1,
                .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR
            }}
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
        .size       = sizeof(PaintJitterSeed),
    }; 

    const Obdn_R_PipelineLayoutInfo pipeLayoutInfos[] = {{
        .descriptorSetCount = 1, 
        .descriptorSetLayouts = descriptorSetLayouts,
    },{
        .descriptorSetCount = 3, 
        .descriptorSetLayouts = descriptorSetLayouts,
        .pushConstantCount = 1,
        .pushConstantsRanges = &pcRange
    },{
        .descriptorSetCount = 1, 
        .descriptorSetLayouts = &descriptorSetLayouts[DESC_SET_POST]
    },{
        .descriptorSetCount = 1, 
        .descriptorSetLayouts = &descriptorSetLayouts[DESC_SET_COMP]
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

    brushRegion = obdn_v_RequestBufferRegion(sizeof(UboBrush), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            OBDN_V_MEMORY_HOST_GRAPHICS_TYPE);
    UboBrush* brush = (UboBrush*)brushRegion.hostData;
    memset(brush, 0, sizeof(UboBrush));
    brush->radius = 0.01;
    brush->mode = 1;

    selectionRegion = obdn_v_RequestBufferRegion(sizeof(Selection), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, OBDN_V_MEMORY_HOST_GRAPHICS_TYPE);


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
        .vertShader = SPVDIR"/raster-vert.spv", 
        .fragShader = SPVDIR"/raster-frag.spv"
    },{
        // post
        .renderPass = postRenderPass,
        .layout     = pipelineLayouts[LAYOUT_POST],
        .sampleCount = VK_SAMPLE_COUNT_1_BIT,
        .frontFace   = VK_FRONT_FACE_CLOCKWISE,
        .blendMode   = OBDN_R_BLEND_MODE_OVER,
        .vertShader = obdn_r_FullscreenTriVertShader(),
        .fragShader = SPVDIR"/post-frag.spv"
    }};

    obdn_r_CreateGraphicsPipelines(OBDN_ARRAY_SIZE(pipeInfosGraph), pipeInfosGraph, graphicsPipelines);
}

static void initRayTracePipelinesAndShaderBindingTables(void)
{
    const Obdn_R_RayTracePipelineInfo pipeInfosRT[] = {{
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
            SPVDIR"/paint-vec2-rchit.spv"
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

    assert(OBDN_ARRAY_SIZE(pipeInfosRT) == RT_PIPELINE_COUNT);

    obdn_r_CreateRayTracePipelines(OBDN_ARRAY_SIZE(pipeInfosRT), pipeInfosRT, raytracePipelines, shaderBindingTables);
}

static void initPaintPipelines(const Obdn_R_BlendMode blendMode)
{
    assert(blendMode != OBDN_R_BLEND_MODE_NONE);

    const Obdn_R_GraphicsPipelineInfo pipeInfo1 = {
        .layout  = pipelineLayouts[LAYOUT_COMP],
        .renderPass = compositeRenderPass, 
        .subpass = 0,
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .sampleCount = VK_SAMPLE_COUNT_1_BIT,
        .viewportDim = {textureSize, textureSize},
        .blendMode   = blendMode,
        .vertShader = obdn_r_FullscreenTriVertShader(),
        .fragShader = SPVDIR"/comp-frag.spv"
    };

    const Obdn_R_GraphicsPipelineInfo pipeInfo2 = {
        .layout  = pipelineLayouts[LAYOUT_COMP],
        .renderPass = compositeRenderPass, 
        .subpass = 1,
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .sampleCount = VK_SAMPLE_COUNT_1_BIT,
        .viewportDim = {textureSize, textureSize},
        .blendMode   = OBDN_R_BLEND_MODE_OVER_STRAIGHT,
        .vertShader = obdn_r_FullscreenTriVertShader(),
        .fragShader = SPVDIR"/comp2a-frag.spv"
    };

    const Obdn_R_GraphicsPipelineInfo pipeInfo3 = {
        .layout  = pipelineLayouts[LAYOUT_COMP],
        .renderPass = compositeRenderPass, 
        .subpass = 2,
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .sampleCount = VK_SAMPLE_COUNT_1_BIT,
        .viewportDim = {textureSize, textureSize},
        .blendMode   = OBDN_R_BLEND_MODE_OVER_STRAIGHT,
        .vertShader = obdn_r_FullscreenTriVertShader(),
        .fragShader = SPVDIR"/comp3a-frag.spv"
    };

    const Obdn_R_GraphicsPipelineInfo pipeInfo4 = {
        .layout  = pipelineLayouts[LAYOUT_COMP],
        .renderPass = compositeRenderPass, 
        .subpass = 3,
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .sampleCount = VK_SAMPLE_COUNT_1_BIT,
        .viewportDim = {textureSize, textureSize},
        .blendMode   = OBDN_R_BLEND_MODE_OVER_STRAIGHT,
        .vertShader = obdn_r_FullscreenTriVertShader(),
        .fragShader = SPVDIR"/comp4a-frag.spv"
    };

    const Obdn_R_GraphicsPipelineInfo pipeInfoSingle = {
        .layout  = pipelineLayouts[LAYOUT_COMP],
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

    obdn_r_CreateGraphicsPipelines(OBDN_ARRAY_SIZE(infos), infos, &graphicsPipelines[PIPELINE_COMP_1]);
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
        const Obdn_R_Frame* frame = obdn_r_GetFrame(i);

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
            .height = textureSize,
            .width  = textureSize,
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
            pipelineLayouts[LAYOUT_COMP], 
            0, 1, &description.descriptorSets[DESC_SET_COMP], 
            0, NULL);

        vkCmdBindPipeline(cmd.buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipelines[PIPELINE_COMP_SINGLE]);

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

        vkCmdBindDescriptorSets(
            cmd.buffer, 
            VK_PIPELINE_BIND_POINT_GRAPHICS, 
            pipelineLayouts[LAYOUT_COMP], 
            0, 1, &description.descriptorSets[DESC_SET_COMP], 
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

static void onRecreateSwapchain(void)
{
    cleanUpSwapchainDependent();

    initOffscreenAttachments();
    initRasterPipelines();
    initSwapchainDependentFramebuffers();

    if (copySwapToHost)
    {
        const uint64_t size = obdn_r_GetFrame(obdn_r_GetCurrentFrameIndex())->size;
        swapHostBufferColor = obdn_v_RequestBufferRegion(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, OBDN_V_MEMORY_HOST_GRAPHICS_TYPE);
        swapHostBufferDepth = obdn_v_RequestBufferRegion(depthAttachment.size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, OBDN_V_MEMORY_HOST_GRAPHICS_TYPE);
    }
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
    matrices->view = scene->view;
    matrices->viewInv = m_Invert4x4(&scene->view);
}

static void updateProj(void)
{
    UboMatrices* matrices = (UboMatrices*)matrixRegion.hostData;
    matrices->proj = scene->proj;
    matrices->projInv = m_Invert4x4(&scene->proj);
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
    if (scene->paint_mode != PAINT_MODE_ERASE)
        updateBrushColor(scene->brush_r, scene->brush_g, scene->brush_b);
    else
        updateBrushColor(1, 1, 1); // must be white for erase to work
    brush->mode = scene->brush_active ? 0 : 1; // active is 0 for some reason
    brush->radius = scene->brush_radius;
    brush->x = scene->brush_x;
    brush->y = scene->brush_y;
}

static void updatePaintMode(void)
{
    vkDeviceWaitIdle(device);
    destroyPaintPipelines();
    switch (scene->paint_mode)
    {
        case PAINT_MODE_OVER:  initPaintPipelines(OBDN_R_BLEND_MODE_OVER); break;
        case PAINT_MODE_ERASE: initPaintPipelines(OBDN_R_BLEND_MODE_ERASE); break;
    }
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
            obdn_r_RecreateSwapchain();
        }
        if (scene->dirt & SCENE_UNDO_BIT)
        {
            if (undo())
                semaphore = acquireImageCommand.semaphore;
        }
        if (scene->dirt & SCENE_LAYER_BACKUP_BIT)
        {
            backupLayer();
            semaphore = acquireImageCommand.semaphore;
        }
        if (scene->dirt & SCENE_LAYER_CHANGED_BIT)
        {
            onLayerChange(scene->layer);
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

    const VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtprops = obdn_v_GetPhysicalDeviceRayTracingProperties();
    const VkDeviceSize progSize = rtprops.shaderGroupBaseAlignment;
    const VkDeviceSize baseAlignment = shaderBindingTables[SBT_SELECT].bufferRegion.offset;
    const VkDeviceSize rayGenOffset   = baseAlignment + 0;
    const VkDeviceSize missOffset     = baseAlignment + 1u * progSize;
    const VkDeviceSize hitGroupOffset = baseAlignment + 2u * progSize; // have to jump over 1 miss shaders

    assert( rayGenOffset % rtprops.shaderGroupBaseAlignment == 0 );

    VkBufferDeviceAddressInfo addrInfo = {
        .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = shaderBindingTables[SBT_SELECT].bufferRegion.buffer,
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

    const VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtprops = obdn_v_GetPhysicalDeviceRayTracingProperties();
    const VkDeviceSize progSize = rtprops.shaderGroupBaseAlignment;
    const VkDeviceSize baseAlignment = shaderBindingTables[SBT_RAY_TRACE].bufferRegion.offset;
    const VkDeviceSize rayGenOffset   = baseAlignment + 0;
    const VkDeviceSize missOffset     = baseAlignment + 1u * progSize;
    const VkDeviceSize hitGroupOffset = baseAlignment + 2u * progSize; // have to jump over 1 miss shaders

    assert( rayGenOffset % rtprops.shaderGroupBaseAlignment == 0 );

    VkBufferDeviceAddressInfo addrInfo = {
        .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = shaderBindingTables[SBT_RAY_TRACE].bufferRegion.buffer,
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

    PaintJitterSeed seed = {
        .x = coal_Rand(),
        .y = coal_Rand()
    };

    vkCmdPushConstants(cmdBuf, pipelineLayouts[LAYOUT_RAYTRACE], VK_SHADER_STAGE_RAYGEN_BIT_KHR, 0, sizeof(PaintJitterSeed), &seed);

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
        .clearValueCount = OBDN_ARRAY_SIZE(clears),
        .pClearValues = clears,
        .renderArea = {{0, 0}, {textureSize, textureSize}},
        .renderPass =  compositeRenderPass,
        .framebuffer = compositeFrameBuffer
    };

    vkCmdBeginRenderPass(cmdBuf, &rpass, VK_SUBPASS_CONTENTS_INLINE);

        vkCmdBindDescriptorSets(
            cmdBuf, 
            VK_PIPELINE_BIND_POINT_GRAPHICS, 
            pipelineLayouts[LAYOUT_COMP], 
            0, 1, &description.descriptorSets[DESC_SET_COMP], 
            0, NULL);

        vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipelines[PIPELINE_COMP_1]);

        vkCmdDraw(cmdBuf, 3, 1, 0, 0);

        vkCmdNextSubpass(cmdBuf, VK_SUBPASS_CONTENTS_INLINE);

        vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipelines[PIPELINE_COMP_2]);

        vkCmdDraw(cmdBuf, 3, 1, 0, 0);

        vkCmdNextSubpass(cmdBuf, VK_SUBPASS_CONTENTS_INLINE);

        vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipelines[PIPELINE_COMP_3]);

        vkCmdDraw(cmdBuf, 3, 1, 0, 0);

        vkCmdNextSubpass(cmdBuf, VK_SUBPASS_CONTENTS_INLINE);

        vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipelines[PIPELINE_COMP_4]);

        vkCmdDraw(cmdBuf, 3, 1, 0, 0);

    vkCmdEndRenderPass(cmdBuf);
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
            .framebuffer = swapchainFrameBuffers[obdn_r_GetCurrentFrameIndex()]
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
            .framebuffer = postFrameBuffers[obdn_r_GetCurrentFrameIndex()]
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

    paint(cmdBuf);

    comp(cmdBuf);

    rasterize(cmdBuf);

    V_ASSERT( vkEndCommandBuffer(cmdBuf) );
}

void r_InitRenderer(uint32_t texSize)
{
    assert(texSize > 0);
    assert(texSize % 256 == 0);
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

    obdn_r_RegisterSwapchainRecreationFn(onRecreateSwapchain);

    assert(imageA.size > 0);

    l_Init(imageA.size); // eventually will move this out
    u_InitUndo(imageB.size);
    onLayerChange(0);
    
    if (copySwapToHost)
    {
        VkDeviceSize swapImageSize = obdn_r_GetFrame(0)->size;
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
    uint32_t i = obdn_r_RequestFrame();

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

            const Image* swapImage = obdn_r_GetFrame(i);

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
        obdn_r_PresentFrame(waitSemaphore);
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
    obdn_r_UnregisterSwapchainRecreateFn(onRecreateSwapchain);
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
    vkDestroyDescriptorPool(device, description.descriptorPool, NULL);
    for (int i = 0; i < description.descriptorSetCount; i++) 
    {
        if (descriptorSetLayouts[i])
            vkDestroyDescriptorSetLayout(device, descriptorSetLayouts[i], NULL);
    }
    memset(&description, 0, sizeof(description));
    vkDestroyRenderPass(device, swapchainRenderPass, NULL);
    vkDestroyRenderPass(device, postRenderPass, NULL);
    vkDestroyRenderPass(device, compositeRenderPass, NULL);
    vkDestroyRenderPass(device, singleCompositeRenderPass, NULL);
    vkDestroyFramebuffer(device, compositeFrameBuffer, NULL);
    vkDestroyFramebuffer(device, backgroundFrameBuffer, NULL);
    vkDestroyFramebuffer(device, foregroundFrameBuffer, NULL);
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

    uint32_t frameId = obdn_r_GetCurrentFrameIndex();
    *colorOffset = obdn_r_GetFrame(frameId)->offset;
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
