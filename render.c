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
    PIPELINE_RASTER,
    PIPELINE_POST,
    G_PIPELINE_COUNT
};

_Static_assert(G_PIPELINE_COUNT < OBDN_MAX_PIPELINES, "must be less than max pipelines");

typedef Obdn_V_BufferRegion BufferRegion;

static BufferRegion  matrixRegion;
static BufferRegion  brushRegion;

static Obdn_R_Primitive     renderPrim;

static VkPipelineLayout           pipelineLayout;
static VkPipeline                 graphicsPipelines[G_PIPELINE_COUNT];

static VkDescriptorSetLayout descriptorSetLayout;
static Obdn_R_Description    description;

static Obdn_V_Image   depthAttachment;

static uint32_t graphicsQueueFamilyIndex;

static Command renderCommand;

static VkFramebuffer   swapchainFrameBuffers[OBDN_FRAME_COUNT];
static VkFramebuffer   postFrameBuffers[OBDN_FRAME_COUNT];

static const VkFormat depthFormat   = VK_FORMAT_D32_SFLOAT;

static VkRenderPass swapchainRenderPass;
static VkRenderPass postRenderPass;

static const PaintScene*   paintScene;
static const Obdn_S_Scene* renderScene;

extern Parms parms;
// swap to host stuff

static bool            copySwapToHost;
static bool            fastPath;
static BufferRegion    swapHostBufferColor;
static BufferRegion    swapHostBufferDepth;
static Command         copyToHostCommand;
static VkSemaphore     extFrameReadSemaphore;

// swap to host stuff

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
}

static void initDescSetsAndPipeLayouts(void)
{
    const Obdn_R_DescriptorSetInfo descSets[] = {{
        //   raster
            .bindingCount = 3, 
            .bindings = {{
                .descriptorCount = 1,
                .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .stageFlags = VK_SHADER_STAGE_VERTEX_BIT
            },{ // brush
                .descriptorCount = 1,
                .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT
            },{ // paint image
                .descriptorCount = 1,
                .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT
            }},
    }};

    assert(OBDN_ARRAY_SIZE(descSets) == 1);
    obdn_r_CreateDescriptionsAndLayouts(OBDN_ARRAY_SIZE(descSets), descSets, &descriptorSetLayout, 1, &description);

    const Obdn_R_PipelineLayoutInfo pipeLayoutInfos[] = {{
        .descriptorSetCount = 1, 
        .descriptorSetLayouts = &descriptorSetLayout,
    }};

    obdn_r_CreatePipelineLayouts(OBDN_ARRAY_SIZE(pipeLayoutInfos), pipeLayoutInfos, &pipelineLayout);
}

static void initUbos(void)
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

static void updateUbos(void)
{
    VkDescriptorBufferInfo uniformInfoMatrices = {
        .range  = matrixRegion.size,
        .offset = matrixRegion.offset,
        .buffer = matrixRegion.buffer,
    };

    VkDescriptorBufferInfo uniformInfoBrush = {
        .range  = matrixRegion.size,
        .offset = matrixRegion.offset,
        .buffer = matrixRegion.buffer,
    };

    VkWriteDescriptorSet writes[] = {{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstArrayElement = 0,
            .dstSet = description.descriptorSets[0],
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .pBufferInfo = &uniformInfoMatrices
    },{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstArrayElement = 0,
            .dstSet = description.descriptorSets[0],
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .pBufferInfo = &uniformInfoBrush
    }};

    vkUpdateDescriptorSets(device, OBDN_ARRAY_SIZE(writes), writes, 0, NULL);
}

static void updatePaintTexture(void)
{
    assert(renderScene->textureCount > 0);

    VkDescriptorImageInfo texInfo = {
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .imageView   = renderScene->textures[0].devImage.view,
        .sampler     = renderScene->textures[0].devImage.sampler
    };

    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstArrayElement = 0,
        .dstSet = description.descriptorSets[0],
        .dstBinding = 1,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &texInfo
    };

    vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
}

static void initRasterPipelines(void)
{
    Obdn_R_AttributeSize attrSizes[3] = {12, 12, 8};
    const Obdn_R_GraphicsPipelineInfo pipeInfosGraph[] = {{
        // raster
        .renderPass = swapchainRenderPass, 
        .layout     = pipelineLayout,
        .vertexDescription = obdn_r_GetVertexDescription(3, attrSizes),
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .sampleCount = VK_SAMPLE_COUNT_1_BIT,
        .viewportDim = {OBDN_WINDOW_WIDTH, OBDN_WINDOW_HEIGHT},
        .vertShader = SPVDIR"/raster-vert.spv", 
        .fragShader = SPVDIR"/raster-frag.spv"
    },{
        // post
        .renderPass = postRenderPass,
        .layout     = pipelineLayout,
        .sampleCount = VK_SAMPLE_COUNT_1_BIT,
        .frontFace   = VK_FRONT_FACE_CLOCKWISE,
        .blendMode   = OBDN_R_BLEND_MODE_OVER,
        .viewportDim = {OBDN_WINDOW_WIDTH, OBDN_WINDOW_HEIGHT},
        .vertShader = obdn_r_FullscreenTriVertShader(),
        .fragShader = SPVDIR"/post-frag.spv"
    }};

    obdn_r_CreateGraphicsPipelines(OBDN_ARRAY_SIZE(pipeInfosGraph), pipeInfosGraph, graphicsPipelines);
}

static void initSwapchainDependentFramebuffers(void)
{
    for (int i = 0; i < OBDN_FRAME_COUNT; i++) 
    {
        const Obdn_R_Frame* frame = obdn_v_GetFrame(i);

        const VkImageView attachments[] = {frame->view, depthAttachment.view};

        VkFramebufferCreateInfo framebufferInfo = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .layers = 1,
            .height = OBDN_WINDOW_HEIGHT,
            .width  = OBDN_WINDOW_WIDTH,
            .renderPass = swapchainRenderPass,
            .attachmentCount = 2,
            .pAttachments = attachments 
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
    matrices->view = paintScene->view;
    matrices->viewInv = m_Invert4x4(&paintScene->view);
}

static void updateProj(void)
{
    UboMatrices* matrices = (UboMatrices*)matrixRegion.hostData;
    matrices->proj = paintScene->proj;
    matrices->projInv = m_Invert4x4(&paintScene->proj);
}

static void updateBrush(void)
{
    UboBrush* brush = (UboBrush*)brushRegion.hostData;
    brush->radius = paintScene->brush_radius;
    brush->x = paintScene->brush_x;
    brush->y = paintScene->brush_y;
}

static VkSemaphore syncScene(const uint32_t frameIndex)
{
    VkSemaphore semaphore = VK_NULL_HANDLE;
    if (paintScene->dirt)
    {
        if (paintScene->dirt & SCENE_VIEW_BIT)
            updateView();
        if (paintScene->dirt & SCENE_PROJ_BIT)
            updateProj();
        if (paintScene->dirt & SCENE_BRUSH_BIT)
            updateBrush();
        if (paintScene->dirt & SCENE_WINDOW_BIT)
        {
            OBDN_WINDOW_WIDTH = paintScene->window_width;
            OBDN_WINDOW_HEIGHT = paintScene->window_height;
            obdn_v_RecreateSwapchain();
        }
    }
    return semaphore;
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
        pipelineLayout,
        0, 1, &description.descriptorSets[0], 
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
            pipelineLayout,
            0, 1, &description.descriptorSets[0],
            0, NULL);

        vkCmdBeginRenderPass(cmdBuf, &rpass, VK_SUBPASS_CONTENTS_INLINE);

            vkCmdDraw(cmdBuf, 3, 1, 0, 0);

        vkCmdEndRenderPass(cmdBuf);
    }
}

static void updateRenderCommands(const int8_t frameIndex)
{
    VkCommandBuffer cmdBuf = renderCommand.buffer;

    obdn_v_BeginCommandBuffer(cmdBuf);

    rasterize(cmdBuf);

    V_ASSERT( vkEndCommandBuffer(cmdBuf) );
}

void r_InitRenderer()
{
    graphicsQueueFamilyIndex = obdn_v_GetQueueFamilyIndex(OBDN_V_QUEUE_GRAPHICS_TYPE);

    copySwapToHost = parms.copySwapToHost; //note this

    renderCommand = obdn_v_CreateCommand(OBDN_V_QUEUE_GRAPHICS_TYPE);

    initOffscreenAttachments();

    initRenderPasses();
    initDescSetsAndPipeLayouts();
    initRasterPipelines();

    initUbos();
    updateUbos();
    updatePaintTexture();

    initSwapchainDependentFramebuffers();

    obdn_v_RegisterSwapchainRecreationFn(onRecreateSwapchain);

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
    obdn_v_WaitForFence(&renderCommand.fence);
    obdn_v_ResetCommand(&renderCommand);
    updateRenderCommands(i);
    obdn_v_SubmitGraphicsCommand(0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 
            waitSemaphore, renderCommand.semaphore, 
            renderCommand.fence, renderCommand.buffer);
    waitSemaphore = obdn_u_Render(renderCommand.semaphore);
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
    obdn_v_WaitForFenceNoReset(&renderCommand.fence);
}

void r_BindScene(const PaintScene* scene_)
{
    paintScene = scene_;
    printf("Scene bound!\n");
}

void r_LoadPrim(Obdn_R_Primitive prim)
{
    assert(renderPrim.vertexRegion.size == 0);
    renderPrim = prim;
}

void r_ClearPrim(void)
{
    obdn_r_FreePrim(&renderPrim);
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
    for (int i = 0; i < G_PIPELINE_COUNT; i++)  // first 2 handles in swapcleanup
    {
        vkDestroyPipeline(device, graphicsPipelines[i], NULL);
    }
    obdn_v_DestroyCommand(renderCommand);
    vkDestroyDescriptorSetLayout(device, descriptorSetLayout, NULL);
    obdn_r_DestroyDescription(&description);
    memset(&description, 0, sizeof(description));
    vkDestroyRenderPass(device, swapchainRenderPass, NULL);
    vkDestroyRenderPass(device, postRenderPass, NULL);
    if (copySwapToHost)
    {
        vkDestroySemaphore(device, extFrameReadSemaphore, NULL);
        obdn_v_DestroyCommand(copyToHostCommand);
    }
    obdn_v_FreeBufferRegion(&matrixRegion);
    obdn_v_FreeBufferRegion(&brushRegion);
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
