#ifndef LAYER_H
#define LAYER_H

#include <tanto/v_memory.h>

#define MAX_LAYERS 64

typedef void (*CreateLayerCallbackFn)(void);

// returns number of layer or -1 on failure
void        l_Init(VkDeviceSize textureSize);
void        l_CleanUp(void);
void        l_SetCreateLayerCallback(CreateLayerCallbackFn fn);
int         l_CreateLayer(void);
void        l_SetActiveLayer(uint16_t id);
uint16_t    l_GetActiveLayer(void);
int         l_GetLayerCount(void);
Tanto_V_BufferRegion* l_GetActiveLayerBufferRegion(void);

#endif /* end of include guard: LAYER_H */
