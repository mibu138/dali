#include "layer.h"
#include "tanto/v_memory.h"
#include <tanto/r_renderpass.h>
#include <tanto/v_video.h>
#include <tanto/v_image.h>
#include <tanto/t_def.h>
#include <string.h>
#include <vulkan/vulkan_core.h>

typedef struct {
    Tanto_V_BufferRegion bufferRegion;
} Layer;

struct {
    uint16_t layerCount;
    uint16_t activeLayer;
    Layer    layers[MAX_LAYERS];
    Tanto_V_BufferRegion backBuffer;
    Tanto_V_BufferRegion frontBuffer;
} layerStack;

static VkDeviceSize textureSize;

CreateLayerCallbackFn onCreateLayer;

void l_Init(VkDeviceSize size)
{
    textureSize = size;

    layerStack.backBuffer  = tanto_v_RequestBufferRegion(textureSize, 
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 
            TANTO_V_MEMORY_HOST_TYPE);

    layerStack.frontBuffer = tanto_v_RequestBufferRegion(textureSize, 
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 
            TANTO_V_MEMORY_HOST_TYPE);

    l_CreateLayer(); // create one layer to start
}

void l_CleanUp()
{
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
    const uint16_t curId = layerStack.layerCount++;

    layerStack.layers[curId].bufferRegion = tanto_v_RequestBufferRegion(textureSize, 
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 
            TANTO_V_MEMORY_HOST_TYPE);
    
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

Tanto_V_BufferRegion* l_GetActiveLayerBufferRegion(void)
{
    return &layerStack.layers[layerStack.activeLayer].bufferRegion;
}

