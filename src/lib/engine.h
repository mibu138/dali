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

// grimoire is optional
void dali_CreateEngine(const Obdn_Instance* instance, Obdn_Memory* memory,
                       Dali_UndoManager* undo, Obdn_Scene* scene,
                       const Dali_Brush* brush, const uint32_t texSize,
                       Hell_Grimoire* grimoire, Dali_Engine* engine);
VkSemaphore dali_Paint(Dali_Engine* engine, const Obdn_Scene* scene,
                       const Dali_Brush* brush, Dali_LayerStack* stack,
                       Dali_UndoManager* um, VkCommandBuffer cmdbuf);
void        dali_DestroyEngine(Dali_Engine* engine);

Obdn_MaterialHandle dali_GetPaintMaterial(Dali_Engine* engine);

void dali_SetActivePrim(Dali_Engine* engine, Obdn_PrimitiveHandle prim);
Obdn_PrimitiveHandle dali_GetActivePrim(Dali_Engine* engine);

Dali_Engine* dali_AllocEngine(void);

#endif /* end of include guard: PAINT_H */
