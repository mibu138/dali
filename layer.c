#include "layer.h"
#include "tanto/v_memory.h"
#include <tanto/r_renderpass.h>
#include <tanto/v_video.h>
#include <tanto/v_image.h>
#include <tanto/t_def.h>
#include <string.h>
#include <vulkan/vulkan_core.h>

typedef struct {
    Tanto_V_Image image;
    VkFramebuffer frameBuffer;
} Layer;

struct {
    uint16_t layerCount;
    uint16_t activeLayer;
    Layer    layers[MAX_LAYERS];
} layerStack;

static VkExtent2D   imageDimensions;
static VkFormat     imageFormat;
static VkRenderPass applyRenderPass;

CreateLayerCallbackFn onCreateLayer;

static void initCompRenderPass(void)
{
    const VkAttachmentDescription attachmentColor = {
        .flags = 0,
        .format = imageFormat,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };

    const VkAttachmentDescription attachments[] = {
        attachmentColor 
    };

    VkAttachmentReference colorReference = {
        .attachment = 0,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };

    const VkSubpassDescription subpass = {
        .flags                   = 0,
        .pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .inputAttachmentCount    = 0,
        .pInputAttachments       = NULL,
        .colorAttachmentCount    = 1,
        .pColorAttachments       = &colorReference,
        .pResolveAttachments     = NULL,
        .pDepthStencilAttachment = NULL,
        .preserveAttachmentCount = 0,
        .pPreserveAttachments    = NULL,
    };

    Tanto_R_RenderPassInfo rpi = {
        .attachmentCount = 1,
        .pAttachments = attachments,
        .subpassCount = 1,
        .pSubpasses = &subpass,
    };

    tanto_r_CreateRenderPass(&rpi, &applyRenderPass);
}

void l_Init(const VkExtent2D dim, const VkFormat format)
{
    assert(dim.width > 1 && dim.height > 1);
    imageDimensions = dim;
    imageFormat     = format;
    layerStack.layerCount = 0;
    layerStack.activeLayer = 0;

    initCompRenderPass();

    l_CreateLayer(); // create one layer to start
}

void l_CleanUp()
{
    for (int i = 0; i < layerStack.layerCount; i++) 
    {
        tanto_v_FreeImage(&layerStack.layers[i].image);
        vkDestroyFramebuffer(device, layerStack.layers[i].frameBuffer, NULL);
    }
    vkDestroyRenderPass(device, applyRenderPass, NULL);
    memset(&layerStack, 0, sizeof(layerStack));
    onCreateLayer = NULL;
}

void l_SetCreateLayerCallback(CreateLayerCallbackFn fn)
{
    onCreateLayer = fn;
}

int l_CreateLayer(void)
{
    assert(layerStack.layerCount < MAX_LAYERS);
    uint16_t curId = layerStack.layerCount++;
    layerStack.layers[curId].image = tanto_v_CreateImageAndSampler(imageDimensions.width, imageDimensions.height, imageFormat, 
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | 
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 
            VK_IMAGE_ASPECT_COLOR_BIT,
            VK_FILTER_LINEAR, 
            1);

    tanto_v_TransitionImageLayout(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 
            &layerStack.layers[curId].image);

    // may be necesary will have to check the spec to see if images are empty by default
    //tanto_v_ClearColorImage(&textureImage);

    const VkFramebufferCreateInfo framebufferInfo = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .layers = 1,
        .width  = imageDimensions.width,
        .height = imageDimensions.height,
        .renderPass = applyRenderPass,
        .attachmentCount = 1,
        .pAttachments = &layerStack.layers[curId].image.view
    };

    V_ASSERT( vkCreateFramebuffer(device, &framebufferInfo, NULL, &layerStack.layers[curId].frameBuffer) );
    
    if (onCreateLayer)
        onCreateLayer();
    TANTO_DEBUG_PRINT("Layer created!");
    printf("Adding layer. There are now %d layers. Active layer is %d\n", layerStack.layerCount, layerStack.activeLayer);
    return curId;
}

void l_SetActiveLayer(uint16_t id)
{
    assert(id < layerStack.layerCount);
    layerStack.activeLayer = id;
    onCreateLayer(); // should get its own call back but this should work for now
    printf("Setting layer to: %d\n", id);
}

int l_GetLayerCount(void)
{
    return layerStack.layerCount;
}

uint16_t l_GetActiveLayer(void)
{
    return layerStack.activeLayer;
}

VkFramebuffer l_GetActiveFramebuffer(void)
{
    return layerStack.layers[layerStack.activeLayer].frameBuffer;
}

VkRenderPass l_GetRenderPass(void)
{
    return applyRenderPass;
}

VkSampler l_GetSampler(uint16_t layerId)
{
    assert(layerId < layerStack.layerCount);
    return layerStack.layers[layerId].image.sampler;
}

VkImageView l_GetImageView(uint16_t layerId)
{
    assert(layerId < layerStack.layerCount);
    return layerStack.layers[layerId].image.view;
}


