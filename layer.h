#ifndef LAYER_H
#define LAYER_H

#include <vulkan/vulkan_core.h>

#define MAX_LAYERS 64

typedef void (*CreateLayerCallbackFn)(void);

// returns number of layer or -1 on failure
void        l_Init(const VkExtent2D dimensions, const VkFormat format);
void        l_CleanUp(void);
void        l_SetCreateLayerCallback(CreateLayerCallbackFn fn);
int         l_CreateLayer(void);
void        l_SetActiveLayer(uint16_t id);
uint16_t    l_GetActiveLayer(void);
int         l_GetLayerCount(void);
VkFramebuffer l_GetActiveFramebuffer(void);
VkRenderPass l_GetRenderPass(void);
VkSampler   l_GetSampler(uint16_t layerId);
VkImageView l_GetImageView(uint16_t layerId);

#endif /* end of include guard: LAYER_H */
