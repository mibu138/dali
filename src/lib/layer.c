#include "layer.h"
#include "private.h"
#include <obsidian/renderpass.h>
#include <obsidian/image.h>
#include <hell/debug.h>
#include <hell/common.h>
#include "dtags.h"
#include <string.h>

typedef Dali_Layer   Layer;
typedef Dali_LayerId LayerId;

void dali_CreateLayerStack(Obdn_Memory* memory, const VkDeviceSize textureSize, Dali_LayerStack* layerStack)
{
    memset(layerStack, 0, sizeof(Dali_LayerStack));
    layerStack->layerSize  = textureSize;
    layerStack->memory = memory;

    layerStack->backBuffer  = obdn_RequestBufferRegion(memory, textureSize, 
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 
            OBDN_V_MEMORY_HOST_GRAPHICS_TYPE);

    layerStack->frontBuffer = obdn_RequestBufferRegion(memory, textureSize, 
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 
            OBDN_V_MEMORY_HOST_GRAPHICS_TYPE);

    dali_CreateLayer(layerStack); // create one layer to start
}

void dali_DestroyLayerStack(Dali_LayerStack* layerStack)
{
    obdn_FreeBufferRegion(&layerStack->backBuffer);
    obdn_FreeBufferRegion(&layerStack->frontBuffer);
    for (int i = 0; i < layerStack->layerCount; i++)
    {
        obdn_FreeBufferRegion(&layerStack->layers[i].bufferRegion);
    }
    memset(layerStack, 0, sizeof(Dali_LayerStack));
}

int dali_CreateLayer(Dali_LayerStack* layerStack)
{
    assert(layerStack->layerCount < MAX_LAYERS);
    const uint16_t curId = layerStack->layerCount++;

    layerStack->layers[curId].bufferRegion = obdn_RequestBufferRegion(layerStack->memory, layerStack->layerSize, 
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 
            OBDN_V_MEMORY_HOST_GRAPHICS_TYPE);
    
    hell_DebugPrint(PAINT_DEBUG_TAG_LAYER, "Layer created!");
    hell_Print("Adding layer. There are now %d layers. Active layer is %d\n", layerStack->layerCount, layerStack->activeLayer);
    return curId;
}

int dali_GetLayerCount(const Dali_LayerStack* layerStack)
{
    return layerStack->layerCount;
}

LayerId dali_GetActiveLayerId(const Dali_LayerStack* layerStack)
{
    return layerStack->activeLayer;
}

Layer* dali_GetLayer(Dali_LayerStack* layerStack, LayerId id)
{
    assert(id < layerStack->layerCount);
    return &layerStack->layers[id];
}

bool dali_IncrementLayer(Dali_LayerStack* layerStack)
{
    LayerId id = layerStack->activeLayer + 1;
    if (id >= layerStack->layerCount)
        return false;
    else 
    {
        layerStack->activeLayer = id;
        layerStack->dirt |= LAYER_CHANGED_BIT;
        return true;
    }
}

bool dali_DecrementLayer(Dali_LayerStack* layerStack)
{
    LayerId id = layerStack->activeLayer - 1;
    if (id >= layerStack->layerCount) // negatives will wrap around
        return false;
    else 
    {
        layerStack->activeLayer = id;
        layerStack->dirt |= LAYER_CHANGED_BIT;
        return true;
    }
}

uint8_t* dali_CopyTextureToLayer(Dali_LayerStack* layerStack, const LayerId id, const void* data, uint32_t w, uint32_t h, VkFormat format)
{
    assert(id < layerStack->layerCount);
    assert(w == h && w == 4096);
    const uint64_t size = w * h * 4;
    memcpy(layerStack->layers[id].bufferRegion.hostData, data, size);
    return layerStack->layers[id].bufferRegion.hostData;
}

Dali_LayerStack* dali_AllocLayerStack(void)
{
    return hell_Malloc(sizeof(Dali_LayerStack));
}

void dali_LayerStackClearDirt(Dali_LayerStack* layerStack)
{
    layerStack->dirt = 0;
}

void dali_LayerBackup(Dali_LayerStack* layerStack)
{
    layerStack->dirt |= LAYER_BACKUP_BIT;
}
