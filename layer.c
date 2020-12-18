#include "layer.h"
#include "tanto/v_memory.h"
#include <tanto/v_image.h>
#include <string.h>

typedef struct {
    Tanto_V_Image image;
} Layer;

struct {
    uint16_t layerCount;
    uint16_t activeLayer;
    Layer    layers[MAX_LAYERS];
} layerStack;

static VkExtent2D imageDimensions;
static VkFormat   imageFormat;

CreateLayerCallbackFn onCreateLayer;

void l_Init(const VkExtent2D dim, const VkFormat format)
{
    assert(dim.width > 1 && dim.height > 1);
    imageDimensions = dim;
    imageFormat     = format;
    layerStack.layerCount = 0;
    layerStack.activeLayer = 0;

    l_CreateLayer(); // create one layer to start
}

void l_CleanUp()
{
    for (int i = 0; i < layerStack.layerCount; i++) 
    {
        tanto_v_FreeImage(&layerStack.layers[i].image);
    }
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

    // may be necesary will have to check the spec to see if images are empty by default
    //tanto_v_ClearColorImage(&textureImage);
    
    if (onCreateLayer)
        onCreateLayer();
    return curId;
}

void l_SetActiveLayer(uint16_t id)
{
    assert(id < layerStack.layerCount);
    layerStack.activeLayer = id;
}

int l_GetLayerCount(void)
{
    return layerStack.layerCount;
}

uint16_t l_GetActiveLayer(void)
{
    return layerStack.activeLayer;
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
