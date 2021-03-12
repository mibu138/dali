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

typedef Obdn_Mask Scene_DirtMask;

typedef enum {
    SCENE_VIEW_BIT          = (Scene_DirtMask)1 << 0,
    SCENE_PROJ_BIT          = (Scene_DirtMask)1 << 1,
    SCENE_BRUSH_BIT         = (Scene_DirtMask)1 << 2,
    SCENE_PAINT_MODE_BIT    = (Scene_DirtMask)1 << 3,
    SCENE_WINDOW_BIT        = (Scene_DirtMask)1 << 4,
    SCENE_UNDO_BIT          = (Scene_DirtMask)1 << 5,
    SCENE_LAYER_BACKUP_BIT  = (Scene_DirtMask)1 << 6,
    SCENE_LAYER_CHANGED_BIT = (Scene_DirtMask)1 << 7,
} Scene_DirtyBits;

typedef struct {
    Mat4           view;
    Mat4           proj;
    float          brush_x;
    float          brush_y;
    float          brush_radius;
    float          brush_r;
    float          brush_g;
    float          brush_b;
    bool           brush_active;
    float          brush_opacity;
    float          brush_falloff;
    PaintMode      paint_mode;
    Scene_DirtMask dirt;
    L_LayerId      layer;
    uint32_t       window_width;
    uint32_t       window_height;
} PaintScene;

VkSemaphore p_Paint(VkSemaphore waitSemaphore);
void        p_Init(Obdn_S_Scene* sScene, const PaintScene* pScene, const uint32_t texSize);

#endif /* end of include guard: PAINT_H */
