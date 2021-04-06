#ifndef LAYER_H
#define LAYER_H

#include <obsidian/v_memory.h>

#define MAX_LAYERS 64

typedef uint16_t L_LayerId;

typedef struct {
    Obdn_V_BufferRegion bufferRegion;
} L_Layer;

// returns number of layer or -1 on failure
void        l_Init(const VkDeviceSize size);
void        l_CleanUp(void);
int         l_CreateLayer(void);
void        l_SetActiveLayer(uint16_t id);
L_LayerId   l_GetActiveLayerId(void);
int         l_GetLayerCount(void);
L_Layer*    l_GetLayer(L_LayerId id);
bool        l_IncrementLayer(L_LayerId* const id);
bool        l_DecrementLayer(L_LayerId* const id);
// returns address to the layer data
uint8_t*    l_CopyTextureToLayer(const L_LayerId id, const void* data, uint32_t w, uint32_t h, VkFormat format);

#endif /* end of include guard: LAYER_H */
