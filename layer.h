#ifndef LAYER_H
#define LAYER_H

#include <vulkan/vulkan_core.h>

// returns number of layer or -1 on failure
void l_Init(const VkExtent2D dimensions, const VkFormat format);
int  l_CreateLayer(void);
void l_SetActiveLayer(uint16_t id);
int  l_GetLayerCount(void);

#endif /* end of include guard: LAYER_H */
