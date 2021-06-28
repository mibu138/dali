#ifndef PAINT_H
#define PAINT_H

#include "brush.h"
#include "layer.h"
#include "undo.h"
#include <hell/cmd.h>
#include <obsidian/scene.h>

#define IMG_4K 4096
#define IMG_8K IMG_4K * 2
#define IMG_16K IMG_8K * 2

typedef struct Dali_Engine Dali_Engine;

void        dali_CreateEngineAndStack(const Obdn_Instance* instance,
                                      Obdn_Memory*         memory, Hell_Grimoire*,
                                      Dali_UndoManager* undo, Obdn_Scene* sScene,
                                      const Dali_Brush* brush, const uint32_t texSize,
                                      Dali_Engine* engine, Dali_LayerStack* stack);
// returns a semaphore to wait on... dont like this but thats a refactor for another day
VkSemaphore dali_Paint(Dali_Engine* engine, const Obdn_Scene* scene,
           const Dali_Brush* brush, Dali_LayerStack* stack,
           Dali_UndoManager* um, VkCommandBuffer cmdbuf);
void        dali_DestroyEngine(Dali_Engine* engine);

Dali_Engine* dali_AllocEngine(void);

#endif /* end of include guard: PAINT_H */
