#include "layer.h"
#include "obsidian/v_memory.h"
#include <obsidian/r_renderpass.h>
#include <obsidian/v_video.h>
#include <obsidian/v_image.h>
#include <obsidian/t_def.h>
#include <string.h>
#include <vulkan/vulkan_core.h>

typedef L_Layer   Layer;
typedef L_LayerId LayerId;

struct {
    uint16_t layerCount;
    uint16_t activeLayer;
    Layer    layers[MAX_LAYERS];
    Obdn_V_BufferRegion backBuffer;
    Obdn_V_BufferRegion frontBuffer;
} layerStack;

static VkDeviceSize textureSize;

#define MAX_LAYER_CHANGE_FNS 8
uint8_t         layerChangeFnCount;
L_LayerChangeFn onLayerChangeFns[MAX_LAYER_CHANGE_FNS];

void l_Init(const VkDeviceSize size)
{
    textureSize = size;

    layerStack.backBuffer  = obdn_v_RequestBufferRegion(textureSize, 
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 
            OBDN_V_MEMORY_HOST_GRAPHICS_TYPE);

    layerStack.frontBuffer = obdn_v_RequestBufferRegion(textureSize, 
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 
            OBDN_V_MEMORY_HOST_GRAPHICS_TYPE);

    l_CreateLayer(); // create one layer to start
}

void l_RegisterLayerChangeFn(L_LayerChangeFn const fn)
{
    onLayerChangeFns[layerChangeFnCount++] = fn;
    assert(layerChangeFnCount < MAX_LAYER_CHANGE_FNS);
}

void l_CleanUp()
{
    obdn_v_FreeBufferRegion(&layerStack.backBuffer);
    obdn_v_FreeBufferRegion(&layerStack.frontBuffer);
    for (int i = 0; i < layerStack.layerCount; i++)
    {
        obdn_v_FreeBufferRegion(&layerStack.layers[i].bufferRegion);
    }
    memset(&layerStack, 0, sizeof(layerStack));
    memset(onLayerChangeFns, 0, sizeof(onLayerChangeFns));
    layerChangeFnCount = 0;
}

int l_CreateLayer(void)
{
    assert(layerStack.layerCount < MAX_LAYERS);
    const uint16_t curId = layerStack.layerCount++;

    layerStack.layers[curId].bufferRegion = obdn_v_RequestBufferRegion(textureSize, 
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 
            OBDN_V_MEMORY_HOST_GRAPHICS_TYPE);
    
    OBDN_DEBUG_PRINT("Layer created!");
    printf("Adding layer. There are now %d layers. Active layer is %d\n", layerStack.layerCount, layerStack.activeLayer);
    return curId;
}

void l_SetActiveLayer(L_LayerId id)
{
    if (id >= layerStack.layerCount)
    {
        printf("Not enough layers.\n");
        return;
    }
    assert(id < layerStack.layerCount);
    layerStack.activeLayer = id;
    printf("Setting layer to: %d\n", id);
    for (int i = 0; i < layerChangeFnCount; i++) 
    {
        onLayerChangeFns[i](id);
    }
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


