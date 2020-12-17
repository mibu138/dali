#include "layer.h"
#include <tanto/v_image.h>

typedef struct {
    Tanto_V_Image image;
} Layer;

#define MAX_LAYERS 64
struct {
    uint16_t layerCount;
    Layer    layers[MAX_LAYERS];
} layerStack;

static VkExtent2D imageDimensions;
static VkFormat   imageFormat;

void l_Init(const VkExtent2D dim, VkFormat format)
{
    assert(dim.width > 1 && dim.height > 1);
    imageDimensions = dim;
    imageFormat     = format;
    layerStack.layerCount = 0;

    l_CreateLayer(); // create one layer to start
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
    
    return curId;
}
