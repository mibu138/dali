#ifndef PAINT_H
#define PAINT_H

#include <obsidian/v_def.h>
#include <obsidian/s_scene.h>
#include "layer.h"

#define IMG_4K  4096
#define IMG_8K  IMG_4K * 2
#define IMG_16K IMG_8K * 2

typedef enum {
    PAINT_MODE_OVER,
    PAINT_MODE_ERASE
} PaintMode;

typedef Obdn_Mask PaintScene_DirtMask;

typedef enum {
    SCENE_BRUSH_BIT         = (PaintScene_DirtMask)1 << 1,
    SCENE_PAINT_MODE_BIT    = (PaintScene_DirtMask)1 << 2,
    SCENE_UNDO_BIT          = (PaintScene_DirtMask)1 << 3,
    SCENE_LAYER_BACKUP_BIT  = (PaintScene_DirtMask)1 << 4,
    SCENE_LAYER_CHANGED_BIT = (PaintScene_DirtMask)1 << 5,
} Scene_DirtyBits;

typedef struct {
    float               brush_x;
    float               brush_y;
    float               brush_radius;
    float               brush_r;
    float               brush_g;
    float               brush_b;
    bool                brush_active;
    float               brush_opacity;
    float               brush_falloff;
    PaintMode           paint_mode;
    L_LayerId           layer;
    PaintScene_DirtMask dirt;
} PaintScene;

VkSemaphore p_Paint(VkSemaphore waitSemaphore);
void        p_Init(Obdn_S_Scene* sScene, const PaintScene* pScene, const uint32_t texSize);

#endif /* end of include guard: PAINT_H */
