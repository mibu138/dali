#ifndef LAYER_H
#define LAYER_H

#include <obsidian/v_memory.h>

#define MAX_LAYERS 64

typedef uint16_t L_LayerId;
typedef void (*L_LayerChangeFn)(L_LayerId newLayerId);

typedef struct {
    Obdn_V_BufferRegion bufferRegion;
} L_Layer;

// returns number of layer or -1 on failure
void        l_Init(const VkDeviceSize size);
void        l_RegisterLayerChangeFn(L_LayerChangeFn const fn);
void        l_CleanUp(void);
int         l_CreateLayer(void);
void        l_SetActiveLayer(uint16_t id);
uint16_t    l_GetActiveLayerId(void);
int         l_GetLayerCount(void);
L_Layer*    l_GetLayer(L_LayerId id);

#endif /* end of include guard: LAYER_H */
