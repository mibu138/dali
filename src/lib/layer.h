#ifndef LAYER_H
#define LAYER_H

#include <obsidian/memory.h>

typedef uint16_t Dali_LayerId;

typedef struct Dali_Layer Dali_Layer;
typedef struct Dali_LayerStack Dali_LayerStack;

// returns number of layer or -1 on failure
void        dali_CreateLayerStack(Obdn_Memory* memory, const VkDeviceSize size, Dali_LayerStack*);
void        dali_DestroyLayerStack(Dali_LayerStack*);
int         dali_CreateLayer(Dali_LayerStack*);
void        dali_SetActiveLayer(Dali_LayerStack*, uint16_t id);
Dali_LayerId   dali_GetActiveLayerId(const Dali_LayerStack*);
int         dali_GetLayerCount(const Dali_LayerStack*);
Dali_Layer*    dali_GetLayer(Dali_LayerStack*, Dali_LayerId id);
bool        dali_IncrementLayer(Dali_LayerStack*, Dali_LayerId* const id);
bool        dali_DecrementLayer(Dali_LayerStack*, Dali_LayerId* const id);
// returns address to the layer data
uint8_t*    dali_CopyTextureToLayer(Dali_LayerStack*, const Dali_LayerId id, const void* data, uint32_t w, uint32_t h, VkFormat format);

Dali_LayerStack* dali_AllocLayerStack(void);

#endif /* end of include guard: LAYER_H */
