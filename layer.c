#include "layer.h"
#include "tanto/v_memory.h"
#include <tanto/r_renderpass.h>
#include <tanto/v_video.h>
#include <tanto/v_image.h>
#include <tanto/t_def.h>
#include <string.h>
#include <vulkan/vulkan_core.h>

typedef L_Layer   Layer;
typedef L_LayerId LayerId;

struct {
    uint16_t layerCount;
    uint16_t activeLayer;
    Layer    layers[MAX_LAYERS];
    Tanto_V_BufferRegion backBuffer;
    Tanto_V_BufferRegion frontBuffer;
} layerStack;

static VkDeviceSize textureSize;

L_LayerChangeFn onLayerChange;

void l_Init(const VkDeviceSize size, L_LayerChangeFn const fn)
{
    textureSize = size;
    onLayerChange = fn;

    layerStack.backBuffer  = tanto_v_RequestBufferRegion(textureSize, 
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 
            TANTO_V_MEMORY_HOST_GRAPHICS_TYPE);

    layerStack.frontBuffer = tanto_v_RequestBufferRegion(textureSize, 
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 
            TANTO_V_MEMORY_HOST_GRAPHICS_TYPE);

    l_CreateLayer(); // create one layer to start
}

void l_CleanUp()
{
    memset(&layerStack, 0, sizeof(layerStack));
    onLayerChange = NULL;
}

int l_CreateLayer(void)
{
    assert(layerStack.layerCount < MAX_LAYERS);
    const uint16_t curId = layerStack.layerCount++;

    layerStack.layers[curId].bufferRegion = tanto_v_RequestBufferRegion(textureSize, 
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 
            TANTO_V_MEMORY_HOST_GRAPHICS_TYPE);
    
    TANTO_DEBUG_PRINT("Layer created!");
    printf("Adding layer. There are now %d layers. Active layer is %d\n", layerStack.layerCount, layerStack.activeLayer);
    return curId;
}

void l_SetActiveLayer(uint16_t id)
{
    if (id >= layerStack.layerCount)
    {
        printf("Not enough layers.\n");
        return;
    }
    assert(id < layerStack.layerCount);
    layerStack.activeLayer = id;
    printf("Setting layer to: %d\n", id);
    onLayerChange();
}

int l_GetLayerCount(void)
{
    return layerStack.layerCount;
}

uint16_t l_GetActiveLayerId(void)
{
    return layerStack.activeLayer;
}

Layer* l_GetLayer(LayerId id)
{
    assert(id < layerStack.layerCount);
    return &layerStack.layers[id];
}


