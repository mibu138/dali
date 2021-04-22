#include "layer.h"
#include "obsidian/v_memory.h"
#include <obsidian/r_renderpass.h>
#include <obsidian/v_video.h>
#include <obsidian/v_image.h>
#include <hell/debug.h>
#include <hell/common.h>
#include "dtags.h"
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
    l_CreateLayer(); // create one layer to start
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
}

int l_CreateLayer(void)
{
    assert(layerStack.layerCount < MAX_LAYERS);
    const uint16_t curId = layerStack.layerCount++;

    layerStack.layers[curId].bufferRegion = obdn_v_RequestBufferRegion(textureSize, 
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 
            OBDN_V_MEMORY_HOST_GRAPHICS_TYPE);
    
    hell_DebugPrint(PAINT_DEBUG_TAG_LAYER, "Layer created!");
    hell_Print("Adding layer. There are now %d layers. Active layer is %d\n", layerStack.layerCount, layerStack.activeLayer);
    return curId;
}

int l_GetLayerCount(void)
{
    return layerStack.layerCount;
}

LayerId l_GetActiveLayerId(void)
{
    return layerStack.activeLayer;
}

Layer* l_GetLayer(LayerId id)
{
    assert(id < layerStack.layerCount);
    return &layerStack.layers[id];
}

bool l_IncrementLayer(LayerId* const id)
{
    *id = layerStack.activeLayer + 1;
    if (*id >= layerStack.layerCount)
        return false;
    else 
    {
        layerStack.activeLayer = *id;
        return true;
    }
}

bool l_DecrementLayer(LayerId* const id)
{
    *id = layerStack.activeLayer - 1;
    if (*id >= layerStack.layerCount) // negatives will wrap around
        return false;
    else 
    {
        layerStack.activeLayer = *id;
        return true;
    }
}

uint8_t* l_CopyTextureToLayer(const LayerId id, const void* data, uint32_t w, uint32_t h, VkFormat format)
{
    assert(id < layerStack.layerCount);
    assert(w == h && w == 4096);
    const uint64_t size = w * h * 4;
    memcpy(layerStack.layers[id].bufferRegion.hostData, data, size);
    return layerStack.layers[id].bufferRegion.hostData;
}
